/*
 * FPC Disum (10a5:9201) probe state and evidence writer tests.
 *
 * These tests use artificial pixels only.  They do not create an FpContext,
 * open a device, play a sound or spawn a notification: the state helper only
 * decides when the manual probe would act, and the writer only stores metadata.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <glib/gstdio.h>

#include "fpc_disum_proto.h"

#include "../tools/fpc_disum_probe_support.h"

/* -------------------------------------------------------------- helpers */

static FpImage *
make_image (guint8 fill)
{
  FpImage *image = fp_image_new (FPC_DISUM_IMAGE_WIDTH, FPC_DISUM_IMAGE_HEIGHT);
  gsize len = 0;
  guchar *data;

  g_assert_nonnull (image);

  data = (guchar *)fp_image_get_data (image, &len);
  g_assert_cmpuint (len, ==, FPC_DISUM_IMAGE_PIXELS);
  memset (data, fill, len);

  return image;
}

static gboolean
never_called (gpointer user_data)
{
  return G_SOURCE_REMOVE;
}

static void
add_string (GVariantBuilder *builder, const gchar *key, const gchar *value)
{
  g_variant_builder_add (builder, "{sv}", key, g_variant_new_string (value));
}

static GVariant *
make_diagnostic (const gchar *run_id)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  add_string (&builder, "run_id", run_id);
  add_string (&builder, "kind", "complete");
  g_variant_builder_add (&builder, "{sv}", "phase",
      g_variant_new_maybe (G_VARIANT_TYPE_STRING, g_variant_new_string ("init")));
  g_variant_builder_add (&builder, "{sv}", "t_us", g_variant_new_int64 (123456));
  g_variant_builder_add (&builder, "{sv}", "actual_length",
      g_variant_new_maybe (G_VARIANT_TYPE_UINT64, g_variant_new_uint64 (4)));

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static GVariant *
make_diagnostic_with (const gchar *key, GVariant *value)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  add_string (&builder, "run_id", "run-test");
  add_string (&builder, "kind", "complete");
  g_variant_builder_add (&builder, "{sv}", key, value);

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static GVariant *
make_binary (const gchar *path, guint64 size, const gchar *sha256)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  add_string (&builder, "path", path);
  g_variant_builder_add (&builder, "{sv}", "size", g_variant_new_uint64 (size));
  add_string (&builder, "sha256", sha256);

  return g_variant_builder_end (&builder);
}

static GVariant *
make_round (gint index)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "round", g_variant_new_int32 (index));
  add_string (&builder, "logical_status", "ok");
  g_variant_builder_add (&builder, "{sv}", "capture_pass", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&builder, "{sv}", "cleanup_pass", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&builder, "{sv}", "image_width", g_variant_new_int32 (112));
  g_variant_builder_add (&builder, "{sv}", "image_height", g_variant_new_int32 (88));
  g_variant_builder_add (&builder, "{sv}", "image_bytes", g_variant_new_uint64 (9856));
  g_variant_builder_add (&builder, "{sv}", "image_structure_valid", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&builder, "{sv}", "image_distinct", g_variant_new_boolean (FALSE));

  return g_variant_builder_end (&builder);
}

static GVariant *
make_summary (void)
{
  GVariantBuilder builder;
  GVariantBuilder rounds;

  g_variant_builder_init (&rounds, G_VARIANT_TYPE ("aa{sv}"));
  g_variant_builder_add_value (&rounds, make_round (0));
  g_variant_builder_add_value (&rounds, make_round (1));

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  add_string (&builder, "run_id", "run-test");
  add_string (&builder, "spec_sha256", "spec");
  g_variant_builder_add (&builder, "{sv}", "probe_binary", make_binary ("/bin/probe", 42, "aaaa"));
  g_variant_builder_add (
      &builder, "{sv}", "loaded_library", make_binary ("/lib/libfprint.so", 43, "bbbb"));
  g_variant_builder_add (&builder, "{sv}", "rounds", g_variant_builder_end (&rounds));
  g_variant_builder_add (&builder, "{sv}", "evidence_complete", g_variant_new_boolean (TRUE));

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static GVariant *
make_feedback (void)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (
      &builder, "{sv}", "pressed_after_cue", g_variant_new_maybe (G_VARIANT_TYPE_STRING, NULL));
  g_variant_builder_add (
      &builder, "{sv}", "held_until_end", g_variant_new_maybe (G_VARIANT_TYPE_STRING, NULL));
  g_variant_builder_add (
      &builder, "{sv}", "perceived_cue_us", g_variant_new_maybe (G_VARIANT_TYPE_INT64, NULL));
  g_variant_builder_add (
      &builder, "{sv}", "press_us", g_variant_new_maybe (G_VARIANT_TYPE_INT64, NULL));
  g_variant_builder_add (
      &builder, "{sv}", "release_us", g_variant_new_maybe (G_VARIANT_TYPE_INT64, NULL));

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gchar *
fresh_run_path (void)
{
  g_autofree gchar *tmp = g_dir_make_tmp ("fpc-disum-probe-XXXXXX", NULL);

  g_assert_nonnull (tmp);

  /* Keep the temporary parent for cleanup: derive the run path from it. */
  return g_build_filename (tmp, "run", NULL);
}

static guint
count_lines (const gchar *path)
{
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  guint lines = 0;

  g_assert_true (g_file_get_contents (path, &contents, &length, NULL));

  for (gsize i = 0; i < length; i++)
    {
      if (contents[i] == '\n')
        lines++;
    }

  return lines;
}

static void
cleanup_run_path (const gchar *run_path)
{
  g_autofree gchar *parent = g_path_get_dirname (run_path);
  const gchar *names[] = { "run.json", "events.jsonl", "operator-feedback.json" };

  for (gsize i = 0; i < G_N_ELEMENTS (names); i++)
    {
      g_autofree gchar *file = g_build_filename (run_path, names[i], NULL);

      g_remove (file);
    }

  g_rmdir (run_path);
  g_rmdir (parent);
}

/* ------------------------------------------------------------ state tests */

static void
test_combined_flags (void)
{
  FpcDisumProbeCaptureState state = { 0 };

  state.expected = PROBE_EXPECT_IMAGE;

  g_assert_false (fpc_disum_probe_on_finger_flags (&state, FP_FINGER_STATUS_NEEDED));
  g_assert_false (state.finger_seen);

  g_assert_true (
      fpc_disum_probe_on_finger_flags (&state, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT));
  g_assert_true (state.finger_seen);

  /* Duplicate notification: reported once. */
  g_assert_false (
      fpc_disum_probe_on_finger_flags (&state, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT));

  fpc_disum_probe_capture_state_clear (&state);
}

static void
test_arm_lengths (void)
{
  FpcDisumProbeCaptureState state = { 0 };

  state.expected = PROBE_EXPECT_IMAGE;

  g_assert_true (fpc_disum_probe_on_arm (&state, 4));
  g_assert_true (state.cue_sent);
  g_assert_cmpint (state.arm_complete_us, >, 0);

  /* Duplicate ARM notification: no second cue. */
  g_assert_false (fpc_disum_probe_on_arm (&state, 4));

  fpc_disum_probe_capture_state_clear (&state);

  state.expected = PROBE_EXPECT_IMAGE;
  g_assert_false (fpc_disum_probe_on_arm (&state, 3));
  g_assert_false (state.cue_sent);
  g_assert_false (fpc_disum_probe_on_arm (&state, 0));
  g_assert_false (state.cue_sent);
  g_assert_cmpint (state.arm_complete_us, ==, 0);

  fpc_disum_probe_capture_state_clear (&state);
}

static void
test_negative_no_cue (void)
{
  FpcDisumProbeCaptureState state = { 0 };

  state.expected = PROBE_EXPECT_NO_TOUCH;

  /* A successful ARM never asks for a press in the negative mode. */
  g_assert_false (fpc_disum_probe_on_arm (&state, 4));
  g_assert_false (state.cue_sent);
  g_assert_cmpint (state.arm_complete_us, >, 0);

  /* PRESENT is reported, but it does not create a cue either. */
  g_assert_true (fpc_disum_probe_on_finger_flags (&state, FP_FINGER_STATUS_PRESENT));
  g_assert_false (state.cue_sent);

  fpc_disum_probe_capture_state_clear (&state);
}

static void
test_early_termination_removes_timers (void)
{
  FpcDisumProbeCaptureState state = { 0 };
  GCancellable *cancellable = g_cancellable_new ();
  gpointer weak = cancellable;
  guint guard_id, cancel_id, progress_id;

  g_object_add_weak_pointer (G_OBJECT (cancellable), &weak);

  guard_id = g_timeout_add (10000, never_called, NULL);
  cancel_id = g_timeout_add (10000, never_called, NULL);
  progress_id = g_timeout_add (10000, never_called, NULL);

  state.expected = PROBE_EXPECT_IMAGE;
  state.guard_id = guard_id;
  state.cancel_id = cancel_id;
  state.progress_id = progress_id;
  state.cancellable = cancellable;

  fpc_disum_probe_capture_state_clear (&state);

  /* The sources are gone and the owned cancellable was released. */
  g_assert_null (g_main_context_find_source_by_id (NULL, guard_id));
  g_assert_null (g_main_context_find_source_by_id (NULL, cancel_id));
  g_assert_null (g_main_context_find_source_by_id (NULL, progress_id));
  g_assert_null (weak);
  g_assert_null (state.cancellable);
  g_assert_cmpuint (state.guard_id, ==, 0);
  g_assert_cmpuint (state.cancel_id, ==, 0);
  g_assert_cmpuint (state.progress_id, ==, 0);
}

/* -------------------------------------------------------------- run tests */

static void
test_first_failure_last_success_still_fails (void)
{
  FpcDisumProbeRun *run = fpc_disum_probe_run_new ();
  ProbeRoundResult failure = { 0 };
  ProbeRoundResult success = { 0 };

  failure.logical_status = FPC_DISUM_SESSION_ERROR_NO_IMAGE;
  failure.capture_pass = FALSE;
  failure.cleanup_pass = TRUE;
  failure.completion_seen = TRUE;

  success.logical_status = FPC_DISUM_SESSION_OK;
  success.capture_pass = TRUE;
  success.cleanup_pass = TRUE;
  success.completion_seen = TRUE;

  fpc_disum_probe_run_add (run, &failure);
  fpc_disum_probe_run_add (run, &success);

  g_assert_false (fpc_disum_probe_run_passed (run));

  fpc_disum_probe_run_free (run);

  run = fpc_disum_probe_run_new ();
  fpc_disum_probe_run_add (run, &success);
  fpc_disum_probe_run_add (run, &success);
  g_assert_true (fpc_disum_probe_run_passed (run));
  fpc_disum_probe_run_free (run);

  /* No round at all is not a pass. */
  run = fpc_disum_probe_run_new ();
  g_assert_false (fpc_disum_probe_run_passed (run));
  fpc_disum_probe_run_free (run);
}

static void
test_image_distinctness (void)
{
  g_autoptr (FpImage) first = make_image (0x10);
  g_autoptr (FpImage) identical = make_image (0x10);
  g_autoptr (FpImage) different = make_image (0x20);
  FpcDisumProbeRun *run = fpc_disum_probe_run_new ();
  ProbeRoundResult success = { 0 };

  success.logical_status = FPC_DISUM_SESSION_OK;
  success.capture_pass = TRUE;
  success.cleanup_pass = TRUE;
  success.completion_seen = TRUE;

  g_assert_false (fpc_disum_probe_images_distinct (first, identical));
  g_assert_true (fpc_disum_probe_images_distinct (first, different));

  fpc_disum_probe_run_add (run, &success);
  fpc_disum_probe_run_add (run, &success);

  g_assert_false (fpc_disum_probe_reopen_passed (run, first, identical));
  g_assert_true (fpc_disum_probe_reopen_passed (run, first, different));

  fpc_disum_probe_run_free (run);
}

/* ----------------------------------------------------------- output tests */

static void
test_output_permissions_and_refuse_overwrite (void)
{
  g_autofree gchar *run_path = fresh_run_path ();
  g_autoptr (GError) error = NULL;
  FpcDisumProbeOutput *output;
  struct stat st;

  output = fpc_disum_probe_output_new (run_path, "run-test", &error);
  g_assert_no_error (error);
  g_assert_nonnull (output);

  g_assert_cmpint (g_stat (run_path, &st), ==, 0);
  g_assert_cmpuint ((guint)(st.st_mode & 0777), ==, 0700);

  {
    const gchar *names[] = { "run.json", "events.jsonl", "operator-feedback.json" };

    for (gsize i = 0; i < G_N_ELEMENTS (names); i++)
      {
        g_autofree gchar *file = g_build_filename (run_path, names[i], NULL);

        g_assert_cmpint (g_stat (file, &st), ==, 0);
        g_assert_cmpuint ((guint)(st.st_mode & 0777), ==, 0600);
      }
  }

  /* A second run may not append to the first one's evidence. */
  {
    FpcDisumProbeOutput *again = fpc_disum_probe_output_new (run_path, "run-test", &error);

    g_assert_null (again);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error (&error);
  }

  fpc_disum_probe_output_free (output);
  cleanup_run_path (run_path);
}

static void
test_output_rejects_unknown_and_payload_fields (void)
{
  g_autofree gchar *run_path = fresh_run_path ();
  g_autoptr (GError) error = NULL;
  FpcDisumProbeOutput *output;
  g_autoptr (GVariant) valid = make_diagnostic ("run-test");
  g_autoptr (GVariant) payload = make_diagnostic_with ("payload", g_variant_new_string ("00ff00"));
  g_autoptr (GVariant) image_field = make_diagnostic_with ("image", g_variant_new_string ("..."));

  output = fpc_disum_probe_output_new (run_path, "run-test", &error);
  g_assert_no_error (error);

  g_assert_true (fpc_disum_probe_output_write (output, valid, &error));
  g_assert_no_error (error);

  g_assert_false (fpc_disum_probe_output_write (output, payload, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  g_assert_false (fpc_disum_probe_output_write (output, image_field, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  fpc_disum_probe_output_free (output);

  {
    g_autofree gchar *events = g_build_filename (run_path, "events.jsonl", NULL);
    g_autofree gchar *contents = NULL;

    g_assert_true (g_file_get_contents (events, &contents, NULL, NULL));
    g_assert_nonnull (strstr (contents, "\"kind\":\"complete\""));
    g_assert_null (strstr (contents, "payload"));
    g_assert_null (strstr (contents, "\"image\":"));
    g_assert_cmpuint (count_lines (events), ==, 1);
  }

  cleanup_run_path (run_path);
}

static void
test_output_preserves_more_than_256_records (void)
{
  g_autofree gchar *run_path = fresh_run_path ();
  g_autoptr (GError) error = NULL;
  FpcDisumProbeOutput *output;
  g_autoptr (GVariant) diagnostic = make_diagnostic ("run-test");
  g_autoptr (GVariant) summary = make_summary ();
  g_autoptr (GVariant) feedback = make_feedback ();
  g_autofree gchar *events = g_build_filename (run_path, "events.jsonl", NULL);
  g_autofree gchar *run_json = g_build_filename (run_path, "run.json", NULL);
  g_autofree gchar *feedback_json = g_build_filename (run_path, "operator-feedback.json", NULL);
  g_autofree gchar *contents = NULL;
  struct stat st;

  output = fpc_disum_probe_output_new (run_path, "run-test", &error);
  g_assert_no_error (error);

  for (guint i = 0; i < 300; i++)
    {
      g_assert_true (fpc_disum_probe_output_write (output, diagnostic, &error));
      g_assert_no_error (error);
    }

  g_assert_true (fpc_disum_probe_output_finish (output, summary, feedback, &error));
  g_assert_no_error (error);

  fpc_disum_probe_output_free (output);

  g_assert_cmpuint (count_lines (events), ==, 300);

  g_assert_true (g_file_get_contents (run_json, &contents, NULL, NULL));
  g_assert_nonnull (strstr (contents, "\"operator_feedback\""));
  g_assert_nonnull (strstr (contents, "\"evidence_complete\":true"));
  g_clear_pointer (&contents, g_free);

  g_assert_true (g_file_get_contents (feedback_json, &contents, NULL, NULL));
  g_assert_nonnull (strstr (contents, "\"pressed_after_cue\":null"));
  g_clear_pointer (&contents, g_free);

  g_assert_cmpint (g_stat (run_json, &st), ==, 0);
  g_assert_cmpuint ((guint)(st.st_mode & 0777), ==, 0600);

  cleanup_run_path (run_path);
}

static void
test_output_refuses_unknown_summary_keys (void)
{
  g_autofree gchar *run_path = fresh_run_path ();
  g_autoptr (GError) error = NULL;
  FpcDisumProbeOutput *output;
  g_autoptr (GVariant) feedback = make_feedback ();
  GVariantBuilder builder;
  GVariant *summary;
  g_autoptr (GVariant) base = make_summary ();
  GVariantIter iter;
  gchar *key = NULL;
  GVariant *value = NULL;

  output = fpc_disum_probe_output_new (run_path, "run-test", &error);
  g_assert_no_error (error);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_iter_init (&iter, base);
  while (g_variant_iter_next (&iter, "{sv}", &key, &value))
    {
      g_variant_builder_add (&builder, "{sv}", key, value);
      g_variant_unref (value);
      g_free (key);
    }
  g_variant_builder_add (&builder, "{sv}", "payload", g_variant_new_string ("00ff"));
  summary = g_variant_ref_sink (g_variant_builder_end (&builder));

  g_assert_false (fpc_disum_probe_output_finish (output, summary, feedback, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  g_variant_unref (summary);
  fpc_disum_probe_output_free (output);
  cleanup_run_path (run_path);
}

static void
test_output_write_failure (void)
{
  g_autofree gchar *run_path = fresh_run_path ();
  g_autoptr (GError) error = NULL;
  FpcDisumProbeOutput *output;
  g_autoptr (GVariant) diagnostic = make_diagnostic ("run-test");
  struct rlimit previous;
  struct rlimit zero;
  gboolean have_limit;

  output = fpc_disum_probe_output_new (run_path, "run-test", &error);
  g_assert_no_error (error);

  have_limit = getrlimit (RLIMIT_FSIZE, &previous) == 0;

  if (have_limit)
    {
      zero.rlim_cur = 0;
      zero.rlim_max = previous.rlim_max;
      signal (SIGXFSZ, SIG_IGN);
      g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &zero), ==, 0);

      g_assert_false (fpc_disum_probe_output_write (output, diagnostic, &error));
      g_assert_nonnull (error);
      g_clear_error (&error);

      g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &previous), ==, 0);
    }

  if (!have_limit)
    {
      fpc_disum_probe_output_free (output);
      cleanup_run_path (run_path);
      g_test_skip ("RLIMIT_FSIZE could not be inspected");
      return;
    }

  /* The writer stays failed; a summary cannot be claimed as complete. */
  {
    g_autoptr (GVariant) summary = make_summary ();
    g_autoptr (GVariant) feedback = make_feedback ();

    g_assert_false (fpc_disum_probe_output_finish (output, summary, feedback, &error));
    g_assert_nonnull (error);
    g_clear_error (&error);
  }

  fpc_disum_probe_output_free (output);
  cleanup_run_path (run_path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/fpc-disum/probe/combined-flags", test_combined_flags);
  g_test_add_func ("/fpc-disum/probe/arm-lengths", test_arm_lengths);
  g_test_add_func ("/fpc-disum/probe/negative-no-cue", test_negative_no_cue);
  g_test_add_func (
      "/fpc-disum/probe/early-termination-removes-timers", test_early_termination_removes_timers);
  g_test_add_func (
      "/fpc-disum/probe/first-failure-last-success", test_first_failure_last_success_still_fails);
  g_test_add_func ("/fpc-disum/probe/image-distinctness", test_image_distinctness);
  g_test_add_func (
      "/fpc-disum/probe/output-permissions", test_output_permissions_and_refuse_overwrite);
  g_test_add_func (
      "/fpc-disum/probe/output-whitelist", test_output_rejects_unknown_and_payload_fields);
  g_test_add_func (
      "/fpc-disum/probe/output-many-records", test_output_preserves_more_than_256_records);
  g_test_add_func (
      "/fpc-disum/probe/output-summary-whitelist", test_output_refuses_unknown_summary_keys);
  g_test_add_func ("/fpc-disum/probe/output-write-failure", test_output_write_failure);

  return g_test_run ();
}
