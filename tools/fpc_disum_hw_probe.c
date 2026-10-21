/*
 * FPC Disum (10a5:9201) manual hardware probe.
 *
 * This is not a test and must never run automatically: it opens the real
 * sensor, runs the reviewed session through libfprint and reports what
 * happened.  The probe owns every evidence file; the library only forwards
 * de-sensitised completion records and never plays a sound or spawns a
 * notification.
 *
 * Usage: fpc-disum-hw-probe <list|negative|capture|cancel|cancel-reopen|reopen|
 *                             multi-capture>
 *                             [--captures N] [--cancel-after-ms N] [--output-dir PATH]
 *
 * `multi-capture` opens the device once, runs N image rounds in that one
 * session and closes it once: this is the same shape fprintd enroll needs.
 *
 * Exit status: 0 every expected round passed, 1 any round or the evidence
 * failed, 2 bad arguments or an unusable output directory, 130 the operator
 * interrupted the run and the cleanup completed.
 *
 * Reference: docs/CLEANROOM_SPEC.md §4.5 (capture), §6 (initialisation),
 * §16.3 (cancellation and close), appendix C.4 (de-sensitised records).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib-unix.h>
#include <glib.h>
#include <gio/gio.h>

#include "fp-context.h"
#include "fp-device.h"
#include "fp-image.h"

#include "fpc_disum_probe_support.h"
#include "fpc_disum_proto.h"

/* The reviewed delivery this probe was accepted against.  The loaded driver
 * reports the hash of the specification it was built from; comparing the two
 * catches a mismatched probe/library pair before any device I/O. */
#define FPC_DISUM_APPROVED_SPEC_SHA256                                                             \
  "b8feda4fc944d41c78347d5d08927ae86bcec38da35002360666c6bf20387b00"

/* Safety nets; the driver's own windows must fire first (SPEC §16.3). */
#define CAPTURE_GUARD_MS 90000
#define OPEN_GUARD_MS 60000
#define PROBE_MAX_ROUNDS 2
/* One open, N image rounds: the range --captures accepts. */
#define PROBE_MAX_CAPTURES 10
#define PROBE_DEFAULT_CAPTURES 5

typedef enum
{
  PROBE_PHASE_IDLE = 0,
  PROBE_PHASE_OPEN,
  PROBE_PHASE_CAPTURE,
  PROBE_PHASE_CLEANUP,
} ProbePhase;

/* One capture round's own state; the device open/close is not part of it. */
typedef struct
{
  FpcDisumProbeCaptureState capture;
  gboolean active;
  gboolean have_primary;
  FpcDisumSessionStatus logical_status;
} RoundContext;

/* The one cleanup record produced by a close, independent of any round. */
typedef struct
{
  gboolean have_cleanup;
  gboolean cleanup_failed;
} CloseOutcome;

typedef struct
{
  GCancellable *cancellable;
  guint id;
} GuardSource;

typedef struct
{
  gchar *path;
  guint64 size;
  gchar *sha256;
  gboolean verified;
} BinaryIdentity;

static FpcDisumProbeOutput *probe_output;
static gboolean output_broken;
static gboolean correlate_images;

static ProbePhase current_phase;
static GCancellable *current_cancellable;
static gboolean run_cancelled;
static guint sigint_id;
/* Operator report, supplied non-interactively so it is never silently null. */
static const gchar *operator_pressed;
static const gchar *operator_held;

static RoundContext *active_round;
static CloseOutcome *active_close;

/* --------------------------------------------------------------- reporting */

static void
print_error (const gchar *what, const GError *error)
{
  if (error == NULL)
    {
      printf ("%s: ok\n", what);
      return;
    }

  printf ("%s: error domain=%s code=%d message=\"%s\"\n", what, g_quark_to_string (error->domain),
      error->code, error->message);
}

static void
print_write_error (const gchar *what, const GError *error)
{
  fprintf (
      stderr, "evidence: %s failed: %s\n", what, error != NULL ? error->message : "unknown error");
}

/* Only geometry, byte count and the structure flag: never pixels or a digest. */
static void
describe_image (const FpImage *image)
{
  gsize len = 0;
  const guint8 *data;
  guint8 min = 255, max = 0;
  gdouble mean = 0.0, variance = 0.0;

  data = fp_image_get_data ((FpImage *)image, &len);

  for (gsize i = 0; i < len; i++)
    {
      min = MIN (min, data[i]);
      max = MAX (max, data[i]);
      mean += data[i];
    }
  if (len > 0)
    mean /= len;
  for (gsize i = 0; i < len; i++)
    variance += (data[i] - mean) * (data[i] - mean);
  if (len > 0)
    variance /= len;

  printf ("image: %dx%d bytes=%zu structure_valid=%s min=%u max=%u mean=%.1f var=%.1f\n",
      fp_image_get_width ((FpImage *)image), fp_image_get_height ((FpImage *)image), len,
      (fp_image_get_width ((FpImage *)image) == FPC_DISUM_IMAGE_WIDTH
          && fp_image_get_height ((FpImage *)image) == FPC_DISUM_IMAGE_HEIGHT
          && len == FPC_DISUM_IMAGE_PIXELS)
          ? "true"
          : "false",
      min, max, mean, variance);
}

/* ------------------------------------------------------------------- cue */

static gboolean
spawn_notification (const gchar *const *argv, const gchar *name)
{
  g_autoptr (GError) error = NULL;

  if (!g_spawn_async (NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error))
    {
      printf ("cue: %s=failed message=\"%s\"\n", name, error->message);
      fflush (stdout);
      return FALSE;
    }

  printf ("cue: %s=spawned\n", name);
  fflush (stdout);

  return TRUE;
}

/* The device is armed: emit one perceptible cue by argv (no shell), record
 * whether each request started, and never claim a measured display or audio
 * time.  Only the probe does this; the library never plays anything. */
static void
cue_operator (void)
{
  const gchar *notify_argv[] = { "notify-send", "-u", "critical", "-t", "0", "FPC Disum: ARM sent",
    "Press the sensor now and hold it until capture completes", NULL };
  const gchar *bell_argv[] = { "canberra-gtk-play", "-i", "bell", NULL };

  fputs ("*** ARM sent: press the sensor now and hold it ***\n", stdout);
  fflush (stdout);

  spawn_notification (notify_argv, "notify-send");
  spawn_notification (bell_argv, "canberra-gtk-play");
}

/* Diagnostic-only normalized cross-correlation with a small translation
 * search.  It compares two in-memory planes and never stores them. */
static gdouble
image_ncc (const FpImage *a, const FpImage *b)
{
  gsize la = 0, lb = 0;
  const guint8 *da = fp_image_get_data ((FpImage *)a, &la);
  const guint8 *db = fp_image_get_data ((FpImage *)b, &lb);
  guint width = fp_image_get_width ((FpImage *)a);
  guint height = fp_image_get_height ((FpImage *)a);
  gdouble best = -2.0;

  if (la != lb || width != fp_image_get_width ((FpImage *)b)
      || height != fp_image_get_height ((FpImage *)b))
    return -2.0;

  for (gint dy = -6; dy <= 6; dy++)
    for (gint dx = -6; dx <= 6; dx++)
      {
        gdouble sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;

        for (guint y = MAX (0, -dy); y < MIN ((gint)height, (gint)height - dy); y++)
          for (guint x = MAX (0, -dx); x < MIN ((gint)width, (gint)width - dx); x++)
            {
              gdouble va = da[y * width + x];
              gdouble vb = db[(y + dy) * width + (x + dx)];

              sa += va;
              sb += vb;
              saa += va * va;
              sbb += vb * vb;
              sab += va * vb;
              n += 1.0;
            }

        if (n > 0)
          {
            gdouble cov = sab - sa * sb / n;
            gdouble va = saa - sa * sa / n;
            gdouble vb = sbb - sb * sb / n;
            gdouble denom = sqrt (MAX (va * vb, 0.0));

            if (denom > 0 && cov / denom > best)
              best = cov / denom;
          }
      }

  return best;
}

/* ------------------------------------------------------------ round state */

static gboolean
progress_tick (gpointer user_data)
{
  RoundContext *round = user_data;
  gint64 elapsed_s;

  if (!round->active || round->capture.arm_complete_us == 0)
    {
      round->capture.progress_id = 0;
      return G_SOURCE_REMOVE;
    }

  elapsed_s = (g_get_monotonic_time () - round->capture.arm_complete_us) / G_USEC_PER_SEC;
  g_print ("[waiting %" G_GINT64_FORMAT " s of 30 -- keep the finger pressed]\n", elapsed_s);
  fflush (stdout);

  return G_SOURCE_CONTINUE;
}

static gboolean
cancel_timeout_cb (gpointer user_data)
{
  RoundContext *round = user_data;

  round->capture.cancel_id = 0;
  g_cancellable_cancel (round->capture.cancellable);

  return G_SOURCE_REMOVE;
}

static gboolean
guard_timeout_cb (gpointer user_data)
{
  RoundContext *round = user_data;

  round->capture.guard_id = 0;
  printf ("capture: guard timeout, cancelling\n");
  fflush (stdout);
  g_cancellable_cancel (round->capture.cancellable);

  return G_SOURCE_REMOVE;
}

static gboolean
open_guard_cb (gpointer user_data)
{
  GuardSource *guard = user_data;

  guard->id = 0;
  printf ("open: guard timeout, cancelling\n");
  fflush (stdout);
  g_cancellable_cancel (guard->cancellable);

  return G_SOURCE_REMOVE;
}

/* Ctrl-C becomes a run-level cancellation, not a process abort: the current
 * I/O is asked to stop, the cleanup still runs, and the evidence is finished
 * before the probe exits 130. */
static gboolean
sigint_cb (gpointer user_data)
{
  run_cancelled = TRUE;
  printf ("interrupted: run cancelled; waiting for the current operation and cleanup\n");
  fflush (stdout);

  if ((current_phase == PROBE_PHASE_OPEN || current_phase == PROBE_PHASE_CAPTURE)
      && current_cancellable != NULL)
    g_cancellable_cancel (current_cancellable);

  return G_SOURCE_CONTINUE;
}

/* Record fields are nullable; a plain string is accepted too so the reader
 * stays usable for summaries and for older records. */
static gchar *
lookup_nullable_string (GVariant *dict, const gchar *key)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (dict, key, NULL);

  if (value == NULL)
    return NULL;

  if (g_variant_type_is_maybe (g_variant_get_type (value)))
    {
      g_autoptr (GVariant) inner = g_variant_get_maybe (value);

      if (inner == NULL || !g_variant_is_of_type (inner, G_VARIANT_TYPE_STRING))
        return NULL;

      return g_variant_dup_string (inner, NULL);
    }

  if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    return g_variant_dup_string (value, NULL);

  return NULL;
}

/* The diagnostics signal: write every record, and take the round's own
 * termination result and the close's cleanup result from the records. */
static void
on_diagnostic (FpDevice *device, GVariant *record, gpointer user_data)
{
  const gchar *kind = NULL;
  g_autofree gchar *primary = lookup_nullable_string (record, "primary_result");
  g_autofree gchar *cleanup = lookup_nullable_string (record, "cleanup_result");

  if (probe_output != NULL)
    {
      g_autoptr (GError) error = NULL;

      if (!fpc_disum_probe_output_write (probe_output, record, &error))
        {
          output_broken = TRUE;
          print_write_error ("event", error);
        }
    }

  if (!g_variant_is_of_type (record, G_VARIANT_TYPE_VARDICT))
    return;

  g_variant_lookup (record, "kind", "&s", &kind);

  if (active_round != NULL && active_round->active)
    {
      if (primary != NULL)
        {
          FpcDisumSessionStatus status;

          if (fpc_disum_probe_status_from_name (primary, &status))
            {
              active_round->logical_status = status;
              active_round->have_primary = TRUE;
            }
        }
    }

  if (active_close != NULL)
    {
      if (cleanup != NULL)
        {
          active_close->have_cleanup = TRUE;

          if (g_strcmp0 (cleanup, "ok") != 0 && g_strcmp0 (cleanup, "not-needed") != 0)
            active_close->cleanup_failed = TRUE;
        }
    }

  (void)kind;
}

static void
on_arm_complete (FpDevice *device, guint actual_length, gpointer user_data)
{
  RoundContext *round = active_round;

  if (round == NULL || !round->active)
    return;

  if (!fpc_disum_probe_on_arm (&round->capture, actual_length))
    return;

  /* The device is armed and waiting for the touch.  Only an image round asks
   * the operator to press. */
  printf ("arm_complete\n");
  fflush (stdout);

  cue_operator ();

  if (round->capture.progress_id == 0)
    round->capture.progress_id = g_timeout_add_seconds (1, progress_tick, round);
}

static void
on_finger_status_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  RoundContext *round = active_round;

  if (round == NULL || !round->active)
    return;

  /* NEEDED is not a completion and never replays the cue; PRESENT is reported
   * once per round. */
  if (fpc_disum_probe_on_finger_flags (
          &round->capture, fp_device_get_finger_status (FP_DEVICE (object))))
    {
      g_print ("[finger seen by the device -- keep holding]\n");
      fflush (stdout);
    }
}

/* --------------------------------------------------------------- actions */

static gboolean
open_device (FpDevice *device, GCancellable *cancellable)
{
  GuardSource guard = { cancellable, 0 };
  g_autoptr (GError) error = NULL;
  gboolean ok;

  guard.id = g_timeout_add (OPEN_GUARD_MS, open_guard_cb, &guard);

  current_phase = PROBE_PHASE_OPEN;
  current_cancellable = cancellable;
  ok = fp_device_open_sync (device, cancellable, &error);
  current_phase = PROBE_PHASE_IDLE;
  current_cancellable = NULL;

  if (guard.id != 0)
    g_source_remove (guard.id);

  print_error ("open", error);

  return ok;
}

static CloseOutcome
close_device (FpDevice *device)
{
  CloseOutcome outcome = { 0 };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;

  /* The capture's cancellable may already be cancelled; cleanup must still be
   * allowed to run to completion. */
  current_phase = PROBE_PHASE_CLEANUP;
  current_cancellable = NULL;

  active_close = &outcome;

  if (!fp_device_close_sync (device, cancellable, &error))
    print_error ("close", error);
  else
    printf ("close: ok\n");

  active_close = NULL;

  fflush (stdout);
  current_phase = PROBE_PHASE_IDLE;

  return outcome;
}

static gboolean
image_structure_valid (const FpImage *image)
{
  gsize len = 0;

  if (image == NULL)
    return FALSE;

  fp_image_get_data ((FpImage *)image, &len);

  return fp_image_get_width ((FpImage *)image) == FPC_DISUM_IMAGE_WIDTH
         && fp_image_get_height ((FpImage *)image) == FPC_DISUM_IMAGE_HEIGHT
         && len == FPC_DISUM_IMAGE_PIXELS;
}

/*
 * The activation/capture/collect half of a round.  The device must already be
 * open; nothing here opens, closes or reinitialises it.  The round's logical
 * status comes from its own completion record, never from error text.
 * `capture_pass` covers only what this round can observe; the caller folds in
 * the close result when the round owns an open/close pair.
 */
static ProbeRoundResult
capture_round (FpDevice *device, ProbeRoundExpectation expected, guint cancel_after_ms)
{
  ProbeRoundResult result = { 0 };
  RoundContext round = { 0 };
  g_autoptr (GError) error = NULL;
  FpImage *image = NULL;

  result.logical_status = FPC_DISUM_SESSION_ERROR_IO;

  round.capture.expected = expected;
  round.capture.cancellable = g_cancellable_new ();
  round.active = TRUE;
  active_round = &round;

  if (expected == PROBE_EXPECT_CANCELLED && cancel_after_ms > 0)
    round.capture.cancel_id = g_timeout_add (cancel_after_ms, cancel_timeout_cb, &round);

  if (expected == PROBE_EXPECT_IMAGE)
    {
      printf ("capture: waiting for the finger (driver window 30000 ms)\n");
      fflush (stdout);
    }

  round.capture.guard_id = g_timeout_add (CAPTURE_GUARD_MS, guard_timeout_cb, &round);

  current_phase = PROBE_PHASE_CAPTURE;
  current_cancellable = round.capture.cancellable;
  image = fp_device_capture_sync (device, TRUE, round.capture.cancellable, &error);
  current_phase = PROBE_PHASE_IDLE;
  current_cancellable = NULL;

  if (round.capture.guard_id != 0)
    {
      g_source_remove (round.capture.guard_id);
      round.capture.guard_id = 0;
    }

  if (image != NULL)
    describe_image (image);
  else
    print_error ("capture", error);

  result.image = image;
  result.image_structure_valid = image_structure_valid (image);
  result.completion_seen = round.have_primary;

  if (round.have_primary)
    result.logical_status = round.logical_status;

  switch (expected)
    {
    case PROBE_EXPECT_IMAGE:
      result.capture_pass = round.have_primary && result.logical_status == FPC_DISUM_SESSION_OK
                            && result.image_structure_valid;
      break;

    case PROBE_EXPECT_NO_TOUCH:
      /* An unexpected image is a failure; it is never reinterpreted as a
       * capture round. */
      result.capture_pass = round.have_primary
                            && result.logical_status == FPC_DISUM_SESSION_ERROR_NO_TOUCH
                            && image == NULL;
      break;

    case PROBE_EXPECT_CANCELLED:
      result.capture_pass = round.have_primary
                            && result.logical_status == FPC_DISUM_SESSION_ERROR_CANCELLED
                            && image == NULL;
      break;
    }

  round.active = FALSE;
  active_round = NULL;
  fpc_disum_probe_capture_state_clear (&round.capture);

  return result;
}

/*
 * One self-contained round for the legacy modes: open, exactly one capture with
 * the expected outcome, then close.  Completion and cleanup are combined only
 * here so the single-round verdict is exactly what it was before.
 */
static ProbeRoundResult
capture_once (FpDevice *device, ProbeRoundExpectation expected, guint cancel_after_ms)
{
  ProbeRoundResult result = { 0 };
  CloseOutcome close;
  g_autoptr (GCancellable) open_cancellable = g_cancellable_new ();

  result.logical_status = FPC_DISUM_SESSION_ERROR_IO;

  if (!open_device (device, open_cancellable))
    {
      printf ("capture: not started because open failed\n");
      return result;
    }

  result = capture_round (device, expected, cancel_after_ms);

  /* The cleanup is part of the round and must complete even after a cancel. */
  close = close_device (device);

  result.cleanup_pass = close.have_cleanup && !close.cleanup_failed;
  result.completion_seen = result.completion_seen && close.have_cleanup;
  result.capture_pass = result.capture_pass && result.cleanup_pass && result.completion_seen;

  return result;
}

static void
print_round_result (ProbeRoundExpectation expected, const ProbeRoundResult *result)
{
  printf ("round-result: expected=%s logical_status=%s cleanup_pass=%s "
          "completion_seen=%s image=%s\n",
      fpc_disum_probe_expectation_name (expected),
      fpc_disum_probe_logical_status_name (result->logical_status),
      result->cleanup_pass ? "true" : "false", result->completion_seen ? "true" : "false",
      result->image != NULL ? "present" : "absent");
  fflush (stdout);
}

/*
 * One line per multi-capture round.  The cleanup is deliberately absent: it is
 * not known per round because the device is closed once, after the last round.
 */
static void
print_multi_round (guint index, guint captures, const ProbeRoundResult *result)
{
  printf ("round-result: round=%u/%u expected=image logical_status=%s image=%s structure=%s\n",
      index, captures, fpc_disum_probe_logical_status_name (result->logical_status),
      result->image != NULL ? "present" : "absent",
      result->image_structure_valid ? "true" : "false");
  fflush (stdout);
}

static void
print_multi_skipped (guint index, guint captures)
{
  printf ("round-result: round=%u/%u expected=image logical_status=not-run image=absent "
          "structure=false\n",
      index, captures);
  fflush (stdout);
}

/*
 * One open, N image rounds, one close.  A failed round ends the sequence: the
 * session's failure is terminal for the open, so later rounds are reported as
 * not run rather than reusing a stale result or image.  Every requested index
 * still gets exactly one round-result line.
 */
static gboolean
multi_capture (FpDevice *device, guint captures, ProbeRoundResult *results, gsize *out_rounds)
{
  CloseOutcome close = { 0 };
  g_autoptr (GCancellable) open_cancellable = g_cancellable_new ();
  gsize rounds = 0;
  guint images = 0;
  gboolean all_ok = TRUE;
  gboolean cleanup_ok;
  const gchar *cleanup_text;

  *out_rounds = 0;

  if (!open_device (device, open_cancellable))
    {
      printf ("multi-capture: not started because open failed\n");
      return FALSE;
    }

  for (guint i = 0; i < captures; i++)
    {
      if (run_cancelled)
        {
          all_ok = FALSE;
          break;
        }

      results[rounds] = capture_round (device, PROBE_EXPECT_IMAGE, 0);
      print_multi_round (rounds + 1, captures, &results[rounds]);

      if (results[rounds].image != NULL && results[rounds].image_structure_valid
          && results[rounds].logical_status == FPC_DISUM_SESSION_OK)
        images++;

      rounds++;

      if (!results[rounds - 1].capture_pass)
        {
          all_ok = FALSE;
          break;
        }
    }

  for (guint i = rounds; i < captures; i++)
    print_multi_skipped (i + 1, captures);

  /* Exactly one close for the whole open, after the last round. */
  close = close_device (device);

  cleanup_ok = close.have_cleanup && !close.cleanup_failed;
  cleanup_text = !close.have_cleanup ? "missing" : (cleanup_ok ? "ok" : "failed");

  if (correlate_images && images >= 2)
    {
      for (guint i = 0; i < images; i++)
        for (guint j = i + 1; j < images; j++)
          if (results[i].image != NULL && results[j].image != NULL)
            printf ("correlation: %u/%u %.3f\n", i + 1, j + 1,
                image_ncc (results[i].image, results[j].image));
    }

  printf ("multi-capture: images=%u/%u cleanup=%s\n", images, captures, cleanup_text);
  fflush (stdout);

  *out_rounds = rounds;

  for (gsize i = 0; i < rounds; i++)
    {
      /* The one close belongs to every round of this open. */
      results[i].cleanup_pass = cleanup_ok;
      results[i].completion_seen = results[i].completion_seen && close.have_cleanup;
      results[i].capture_pass
          = results[i].capture_pass && results[i].cleanup_pass && results[i].completion_seen;
    }

  return all_ok && rounds == captures && cleanup_ok;
}

/* ------------------------------------------------------- binary identity */

static BinaryIdentity
identify_binary (const gchar *fallback_path)
{
  BinaryIdentity identity = { 0 };
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  struct stat st;

  identity.path = g_strdup (fallback_path);
  identity.sha256 = g_strdup ("unknown");

  if (identity.path == NULL || !g_file_test (identity.path, G_FILE_TEST_EXISTS)
      || stat (identity.path, &st) != 0)
    return identity;

  identity.size = (guint64)st.st_size;

  if (!g_file_get_contents (identity.path, &contents, &length, NULL))
    return identity;

  g_free (identity.sha256);
  identity.sha256
      = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *)contents, length);
  identity.verified = TRUE;

  return identity;
}

static BinaryIdentity
identify_probe_binary (void)
{
  g_autofree gchar *exe = g_file_read_link ("/proc/self/exe", NULL);

  return identify_binary (exe != NULL ? exe : "unknown");
}

static BinaryIdentity
identify_loaded_library (void)
{
  Dl_info info;

  memset (&info, 0, sizeof (info));

  if (dladdr ((void *)fp_context_new, &info) == 0 || info.dli_fname == NULL)
    return identify_binary ("unknown");

  return identify_binary (info.dli_fname);
}

static void
binary_identity_clear (BinaryIdentity *identity)
{
  g_free (identity->path);
  g_free (identity->sha256);
  memset (identity, 0, sizeof (*identity));
}

/* -------------------------------------------------------------- summary */

static GVariant *
binary_to_variant (const BinaryIdentity *identity)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "path",
      g_variant_new_string (identity->path != NULL ? identity->path : "unknown"));
  g_variant_builder_add (&builder, "{sv}", "size", g_variant_new_uint64 (identity->size));
  g_variant_builder_add (&builder, "{sv}", "sha256",
      g_variant_new_string (identity->sha256 != NULL ? identity->sha256 : "unknown"));

  /* Floating: the enclosing a{sv} consumes it. */
  return g_variant_builder_end (&builder);
}

static GVariant *
round_to_variant (guint index, const ProbeRoundResult *result, gboolean image_distinct)
{
  GVariantBuilder builder;
  gsize bytes = 0;
  gint width = 0;
  gint height = 0;

  if (result->image != NULL)
    {
      fp_image_get_data (result->image, &bytes);
      width = (gint)fp_image_get_width (result->image);
      height = (gint)fp_image_get_height (result->image);
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "round", g_variant_new_int32 ((gint32)index));
  g_variant_builder_add (&builder, "{sv}", "logical_status",
      g_variant_new_string (fpc_disum_probe_logical_status_name (result->logical_status)));
  g_variant_builder_add (
      &builder, "{sv}", "capture_pass", g_variant_new_boolean (result->capture_pass));
  g_variant_builder_add (
      &builder, "{sv}", "cleanup_pass", g_variant_new_boolean (result->cleanup_pass));
  g_variant_builder_add (&builder, "{sv}", "image_width", g_variant_new_int32 (width));
  g_variant_builder_add (&builder, "{sv}", "image_height", g_variant_new_int32 (height));
  g_variant_builder_add (&builder, "{sv}", "image_bytes", g_variant_new_uint64 (bytes));
  g_variant_builder_add (&builder, "{sv}", "image_structure_valid",
      g_variant_new_boolean (result->image_structure_valid));
  g_variant_builder_add (
      &builder, "{sv}", "image_distinct", g_variant_new_boolean (image_distinct));

  /* Floating: the rounds array consumes it. */
  return g_variant_builder_end (&builder);
}

static GVariant *
build_summary (const gchar *run_id, const gchar *spec_sha256, const BinaryIdentity *probe_binary,
    const BinaryIdentity *loaded_library, gboolean verified, ProbeRoundResult *results,
    gboolean *distinct, gsize n_rounds, gboolean evidence_complete)
{
  GVariantBuilder builder;
  GVariantBuilder rounds;

  g_variant_builder_init (&rounds, G_VARIANT_TYPE ("aa{sv}"));
  for (gsize i = 0; i < n_rounds; i++)
    g_variant_builder_add_value (&rounds, round_to_variant ((guint)i, &results[i], distinct[i]));

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "run_id", g_variant_new_string (run_id));
  g_variant_builder_add (&builder, "{sv}", "spec_sha256", g_variant_new_string (spec_sha256));
  g_variant_builder_add (&builder, "{sv}", "probe_binary", binary_to_variant (probe_binary));
  g_variant_builder_add (&builder, "{sv}", "loaded_library", binary_to_variant (loaded_library));
  g_variant_builder_add (&builder, "{sv}", "rounds", g_variant_builder_end (&rounds));
  g_variant_builder_add (
      &builder, "{sv}", "evidence_complete", g_variant_new_boolean (evidence_complete && verified));

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* ------------------------------------------------------ operator feedback */

static const gchar *
ask_operator (const gchar *question)
{
  gchar buffer[64];

  printf ("%s [y/n/unknown]: ", question);
  fflush (stdout);

  if (fgets (buffer, sizeof (buffer), stdin) == NULL)
    return "unknown";

  g_strchomp (buffer);

  if (g_ascii_strcasecmp (buffer, "y") == 0 || g_ascii_strcasecmp (buffer, "yes") == 0)
    return "yes";
  if (g_ascii_strcasecmp (buffer, "n") == 0 || g_ascii_strcasecmp (buffer, "no") == 0)
    return "no";

  return "unknown";
}

static GVariant *
build_feedback (void)
{
  GVariantBuilder builder;
  gboolean interactive = isatty (STDIN_FILENO);
  const gchar *pressed = operator_pressed;
  const gchar *held = operator_held;

  if (interactive)
    {
      if (pressed == NULL)
        pressed = ask_operator ("Did you press only after the cue?");
      if (held == NULL)
        held = ask_operator ("Did you hold until the capture finished?");
    }

  /* Actual display/audio/contact times are not measured.  They stay null; an
   * operator report is not a physical timestamp. */
  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "pressed_after_cue",
      g_variant_new_maybe (
          G_VARIANT_TYPE_STRING, pressed != NULL ? g_variant_new_string (pressed) : NULL));
  g_variant_builder_add (&builder, "{sv}", "held_until_end",
      g_variant_new_maybe (
          G_VARIANT_TYPE_STRING, held != NULL ? g_variant_new_string (held) : NULL));
  g_variant_builder_add (
      &builder, "{sv}", "perceived_cue_us", g_variant_new_maybe (G_VARIANT_TYPE_INT64, NULL));
  g_variant_builder_add (
      &builder, "{sv}", "press_us", g_variant_new_maybe (G_VARIANT_TYPE_INT64, NULL));
  g_variant_builder_add (
      &builder, "{sv}", "release_us", g_variant_new_maybe (G_VARIANT_TYPE_INT64, NULL));

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* ------------------------------------------------------------------- main */

static void
print_usage (FILE *stream, const gchar *program)
{
  fprintf (stream,
      "usage: %s <list|negative|capture|cancel|cancel-reopen|reopen|multi-capture>"
      " [--captures N] [--cancel-after-ms N] [--pressed-after-cue yes|no|unknown]"
      " [--held-until-end yes|no|unknown] [--output-dir PATH]\n"
      "  multi-capture: one open, N image rounds, one close (default N=%d, range 1..%d)\n"
      "  --correlate: print pairwise normalized correlation for multi-capture\n",
      program, PROBE_DEFAULT_CAPTURES, PROBE_MAX_CAPTURES);
}

static gboolean
is_known_command (const gchar *command)
{
  return g_strcmp0 (command, "list") == 0 || g_strcmp0 (command, "negative") == 0
         || g_strcmp0 (command, "capture") == 0 || g_strcmp0 (command, "cancel") == 0
         || g_strcmp0 (command, "cancel-reopen") == 0 || g_strcmp0 (command, "reopen") == 0
         || g_strcmp0 (command, "multi-capture") == 0;
}

static FpDevice *
find_single_device (FpContext *context)
{
  GPtrArray *devices;
  FpDevice *found = NULL;
  guint matches = 0;
  const gchar *allowlist = g_getenv ("FP_DRIVERS_ALLOWLIST");

  if (allowlist != NULL)
    printf ("note: FP_DRIVERS_ALLOWLIST=%s is set in this environment\n", allowlist);

  fp_context_enumerate (context);
  devices = fp_context_get_devices (context);

  printf ("devices: %u\n", devices->len);
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *device = g_ptr_array_index (devices, i);
      const gchar *driver = fp_device_get_driver (device);

      printf ("  [%u] driver=%s name=\"%s\"\n", i, driver, fp_device_get_name (device));

      if (g_strcmp0 (driver, "fpc_disum") == 0)
        {
          matches++;
          found = device;
        }
    }

  if (matches == 0)
    {
      fprintf (stderr, "error: no fpc_disum device was found\n");
      return NULL;
    }

  if (matches > 1)
    {
      fprintf (
          stderr, "error: %u fpc_disum devices were found; the probe needs exactly one\n", matches);
      return NULL;
    }

  return g_object_ref (found);
}

static gboolean
check_contract (FpDevice *device, gchar **out_run_id, gchar **out_spec_sha256)
{
  GObjectClass *klass = G_OBJECT_GET_CLASS (device);
  g_autofree gchar *run_id = NULL;
  g_autofree gchar *spec_sha256 = NULL;

  if (g_object_class_find_property (klass, "fpi-fpc-disum-run-id") == NULL
      || g_object_class_find_property (klass, "fpi-fpc-disum-spec-sha256") == NULL)
    {
      fprintf (stderr, "probe/library contract mismatch: private properties are missing\n");
      return FALSE;
    }

  if (g_signal_lookup ("arm-complete", G_OBJECT_TYPE (device)) == 0
      || g_signal_lookup ("fpc-disum-diagnostic", G_OBJECT_TYPE (device)) == 0)
    {
      fprintf (stderr, "probe/library contract mismatch: diagnostic signals are missing\n");
      return FALSE;
    }

  g_object_get (device, "fpi-fpc-disum-run-id", &run_id, NULL);
  g_object_get (device, "fpi-fpc-disum-spec-sha256", &spec_sha256, NULL);

  if (run_id == NULL || *run_id == '\0' || spec_sha256 == NULL || *spec_sha256 == '\0')
    {
      fprintf (stderr, "probe/library contract mismatch: identity properties are empty\n");
      return FALSE;
    }

  if (g_strcmp0 (spec_sha256, FPC_DISUM_APPROVED_SPEC_SHA256) != 0)
    {
      fprintf (stderr,
          "probe/library contract mismatch: driver reports spec %s, approved baseline is %s\n",
          spec_sha256, FPC_DISUM_APPROVED_SPEC_SHA256);
      return FALSE;
    }

  *out_run_id = g_steal_pointer (&run_id);
  *out_spec_sha256 = g_steal_pointer (&spec_sha256);

  return TRUE;
}

int
main (int argc, char **argv)
{
  const gchar *command = NULL;
  const gchar *output_option = NULL;
  guint cancel_after_ms = 3000;
  g_autoptr (FpContext) context = NULL;
  g_autoptr (FpDevice) device = NULL;
  g_autofree gchar *run_id = NULL;
  g_autofree gchar *spec_sha256 = NULL;
  g_autofree gchar *default_output = NULL;
  const gchar *output_path;
  g_autoptr (GError) output_error = NULL;
  ProbeRoundExpectation plan[PROBE_MAX_ROUNDS];
  ProbeRoundResult results[PROBE_MAX_CAPTURES] = { 0 };
  gboolean distinct[PROBE_MAX_CAPTURES] = { FALSE };
  guint captures = PROBE_DEFAULT_CAPTURES;
  gboolean captures_seen = FALSE;
  gsize n_rounds = 0;
  gsize planned_rounds = 0;
  FpcDisumProbeRun *run = NULL;
  BinaryIdentity probe_binary = { 0 };
  BinaryIdentity loaded_library = { 0 };
  gboolean evidence_complete = FALSE;
  gboolean all_passed = FALSE;
  int exit_code = 1;

  g_set_prgname ("fpc-disum-hw-probe");

  /* Argument parsing happens before any FpContext exists. */
  if (argc < 2)
    {
      print_usage (stderr, argv[0]);
      return 2;
    }

  for (gint i = 1; i < argc; i++)
    {
      if (g_strcmp0 (argv[i], "--help") == 0 || g_strcmp0 (argv[i], "-h") == 0)
        {
          print_usage (stdout, argv[0]);
          return 0;
        }

      if (g_strcmp0 (argv[i], "--captures") == 0)
        {
          guint64 value;

          if (i + 1 >= argc)
            {
              fprintf (stderr, "--captures needs a value\n");
              return 2;
            }

          value = g_ascii_strtoull (argv[++i], NULL, 10);
          if (value < 1 || value > PROBE_MAX_CAPTURES)
            {
              fprintf (stderr, "--captures must be between 1 and %d\n", PROBE_MAX_CAPTURES);
              return 2;
            }

          captures = (guint)value;
          captures_seen = TRUE;
          continue;
        }

      if (g_strcmp0 (argv[i], "--cancel-after-ms") == 0)
        {
          if (i + 1 >= argc)
            {
              fprintf (stderr, "--cancel-after-ms needs a value\n");
              return 2;
            }
          cancel_after_ms = (guint)g_ascii_strtoull (argv[++i], NULL, 10);
          if (cancel_after_ms == 0)
            {
              fprintf (stderr, "--cancel-after-ms must be positive\n");
              return 2;
            }
          continue;
        }

      if (g_strcmp0 (argv[i], "--pressed-after-cue") == 0
          || g_strcmp0 (argv[i], "--held-until-end") == 0)
        {
          const gchar *value;

          if (i + 1 >= argc)
            {
              fprintf (stderr, "%s needs yes, no or unknown\n", argv[i]);
              return 2;
            }

          value = argv[++i];
          if (g_strcmp0 (value, "yes") != 0 && g_strcmp0 (value, "no") != 0
              && g_strcmp0 (value, "unknown") != 0)
            {
              fprintf (stderr, "%s needs yes, no or unknown\n", argv[i - 1]);
              return 2;
            }

          if (g_strcmp0 (argv[i - 1], "--pressed-after-cue") == 0)
            operator_pressed = value;
          else
            operator_held = value;
          continue;
        }

      if (g_strcmp0 (argv[i], "--correlate") == 0)
        {
          correlate_images = TRUE;
          continue;
        }

      if (g_strcmp0 (argv[i], "--output-dir") == 0)
        {
          if (i + 1 >= argc)
            {
              fprintf (stderr, "--output-dir needs a path\n");
              return 2;
            }
          output_option = argv[++i];
          continue;
        }

      if (command == NULL && argv[i][0] != '-')
        {
          command = argv[i];
          continue;
        }

      fprintf (stderr, "unknown option: %s\n", argv[i]);
      return 2;
    }

  if (command == NULL || !is_known_command (command))
    {
      print_usage (stderr, argv[0]);
      return 2;
    }

  if (captures_seen && g_strcmp0 (command, "multi-capture") != 0)
    {
      fprintf (stderr, "--captures is only valid with multi-capture\n");
      return 2;
    }

  /* The context, not the device, owns the libusb context: keep it for the run. */
  context = fp_context_new ();
  device = find_single_device (context);

  if (device == NULL)
    return 1;

  if (g_strcmp0 (command, "list") == 0)
    return 0;

  if (!check_contract (device, &run_id, &spec_sha256))
    return 1;

  if (output_option != NULL)
    output_path = output_option;
  else
    {
      const gchar *home = g_get_home_dir ();

      if (home == NULL)
        {
          fprintf (stderr, "error: HOME is not set; --output-dir is required\n");
          return 2;
        }

      default_output = g_build_filename (home, "fpc-disum-runs", run_id, NULL);
      output_path = default_output;
    }

  probe_output = fpc_disum_probe_output_new (output_path, run_id, &output_error);
  if (probe_output == NULL)
    {
      fprintf (stderr, "error: unusable output directory '%s': %s\n", output_path,
          output_error != NULL ? output_error->message : "unknown error");
      return 2;
    }

  g_signal_connect (device, "fpc-disum-diagnostic", G_CALLBACK (on_diagnostic), NULL);
  g_signal_connect (device, "arm-complete", G_CALLBACK (on_arm_complete), NULL);
  g_signal_connect (device, "notify::finger-status", G_CALLBACK (on_finger_status_changed), NULL);

  sigint_id = g_unix_signal_add (SIGINT, sigint_cb, NULL);

  if (g_strcmp0 (command, "multi-capture") == 0)
    {
      /* One open, N rounds, one close; the evidence must contain all N. */
      planned_rounds = captures;
    }
  else if (g_strcmp0 (command, "negative") == 0)
    {
      plan[planned_rounds++] = PROBE_EXPECT_NO_TOUCH;
    }
  else if (g_strcmp0 (command, "capture") == 0)
    {
      plan[planned_rounds++] = PROBE_EXPECT_IMAGE;
    }
  else if (g_strcmp0 (command, "cancel") == 0)
    {
      plan[planned_rounds++] = PROBE_EXPECT_CANCELLED;
    }
  else if (g_strcmp0 (command, "cancel-reopen") == 0)
    {
      plan[planned_rounds++] = PROBE_EXPECT_CANCELLED;
      plan[planned_rounds++] = PROBE_EXPECT_IMAGE;
    }
  else if (g_strcmp0 (command, "reopen") == 0)
    {
      plan[planned_rounds++] = PROBE_EXPECT_IMAGE;
      plan[planned_rounds++] = PROBE_EXPECT_IMAGE;
    }

  run = fpc_disum_probe_run_new ();

  if (g_strcmp0 (command, "multi-capture") == 0)
    {
      /* The open/close pair is owned by multi_capture(), not by each round. */
      all_passed = multi_capture (device, captures, results, &n_rounds);

      for (gsize i = 0; i < n_rounds; i++)
        fpc_disum_probe_run_add (run, &results[i]);
    }
  else
    {
      for (gsize i = 0; i < planned_rounds; i++)
        {
          if (run_cancelled)
            break;

          results[n_rounds] = capture_once (device, plan[i], cancel_after_ms);
          print_round_result (plan[i], &results[n_rounds]);
          fpc_disum_probe_run_add (run, &results[n_rounds]);
          n_rounds++;
        }

      if (g_strcmp0 (command, "reopen") == 0)
        {
          /* Two live images are compared in memory; neither is stored or hashed. */
          gboolean distinct_images
              = n_rounds == planned_rounds
                && fpc_disum_probe_images_distinct (results[0].image, results[1].image);

          printf ("image_distinct=%s\n", distinct_images ? "true" : "false");

          if (n_rounds == planned_rounds)
            {
              distinct[0] = distinct_images;
              distinct[1] = distinct_images;
            }

          all_passed = fpc_disum_probe_reopen_passed (run, results[0].image, results[1].image);
        }
      else if (g_strcmp0 (command, "cancel-reopen") == 0)
        {
          /* The first round must be truly cancelled, the second must deliver a
           * valid image; both cleanups must complete. */
          all_passed = fpc_disum_probe_run_passed (run);
        }
      else
        {
          all_passed = fpc_disum_probe_run_passed (run);
        }
    }

  probe_binary = identify_probe_binary ();
  loaded_library = identify_loaded_library ();

  evidence_complete = !output_broken && probe_binary.verified && loaded_library.verified
                      && n_rounds == planned_rounds;

  {
    g_autoptr (GVariant) summary = build_summary (run_id, spec_sha256, &probe_binary,
        &loaded_library, probe_binary.verified && loaded_library.verified, results, distinct,
        n_rounds, evidence_complete);
    g_autoptr (GVariant) feedback = build_feedback ();
    g_autoptr (GError) error = NULL;

    if (!fpc_disum_probe_output_finish (probe_output, summary, feedback, &error))
      {
        output_broken = TRUE;
        print_write_error ("run summary", error);
      }
  }

  if (sigint_id != 0)
    {
      g_source_remove (sigint_id);
      sigint_id = 0;
    }

  fpc_disum_probe_output_free (probe_output);
  probe_output = NULL;

  if (output_broken)
    exit_code = 1;
  else if (run_cancelled)
    exit_code = 130;
  else if (all_passed && evidence_complete)
    exit_code = 0;
  else
    exit_code = 1;

  for (gsize i = 0; i < n_rounds; i++)
    g_clear_object (&results[i].image);

  fpc_disum_probe_run_free (run);
  binary_identity_clear (&probe_binary);
  binary_identity_clear (&loaded_library);

  return exit_code;
}
