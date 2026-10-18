/*
 * FPC Disum (10a5:9201) manual hardware probe.
 *
 * This is not a test and must never run automatically: it opens the real sensor,
 * runs the reviewed session through libfprint and reports what happened.  It
 * prints only non-sensitive summaries (device identity, image geometry, a digest
 * of the pixel plane and error texts).  No image, envelope, key material or raw
 * device log is written anywhere; redirect stdout into a 0600 file inside a 0700
 * directory if a record is wanted.
 *
 * Reference: docs/CLEANROOM_SPEC.md §4.5 (capture), §6 (initialisation),
 * §16.3 (cancellation and close).  Run it only with an authorised scope.
 *
 * Usage: fpc-disum-hw-probe <list|negative|capture|cancel|cancel-reopen|reopen>
 *                             [--cancel-after-ms N]
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <stdio.h>

#include "fp-context.h"
#include "fp-device.h"
#include "fp-image.h"

/* Safety nets; the driver's own windows must fire first (SPEC §16.3). */
#define CAPTURE_GUARD_MS 90000
#define OPEN_GUARD_MS 60000

/* SPEC §4.5 操作前提: a press is wanted only while a capture waits for the
 * finger; capture_once() sets it and the cue handler below reads it. */
static gboolean press_wanted;

static gboolean
cancel_cb (gpointer user_data)
{
  g_cancellable_cancel ((GCancellable *)user_data);

  return G_SOURCE_REMOVE;
}

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
describe_image (const FpImage *image)
{
  gsize len = 0;
  const guchar *data;
  g_autofree gchar *digest = NULL;

  data = fp_image_get_data ((FpImage *)image, &len);
  digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, len);

  printf ("image: %dx%d bytes=%zu sha256=%s\n", fp_image_get_width ((FpImage *)image),
      fp_image_get_height ((FpImage *)image), len, digest);
}

/*
 * The context owns the libusb context behind every device, so it has to outlive
 * the device the caller keeps: pass it in and hold it for the whole run.
 */
static FpDevice *
find_device (FpContext *context)
{
  GPtrArray *devices;
  FpDevice *found = NULL;
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
          if (found == NULL)
            {
              found = g_object_ref (device);
            }
          else
            {
              printf ("warning: more than one fpc_disum device; using the first\n");
            }
        }
    }

  if (found == NULL)
    printf ("error: no fpc_disum device was found\n");

  return found;
}

static gboolean
open_device (FpDevice *device)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  guint guard;

  guard = g_timeout_add (OPEN_GUARD_MS, cancel_cb, cancellable);
  if (fp_device_open_sync (device, cancellable, &error))
    {
      g_source_remove (guard);
      printf ("open: ok\n");
      return TRUE;
    }

  g_source_remove (guard);
  print_error ("open", error);

  return FALSE;
}

static gboolean
close_device (FpDevice *device)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  guint guard;

  guard = g_timeout_add (CAPTURE_GUARD_MS, cancel_cb, cancellable);
  if (fp_device_close_sync (device, cancellable, &error))
    {
      g_source_remove (guard);
      printf ("close: ok\n");
      return TRUE;
    }

  g_source_remove (guard);
  print_error ("close", error);

  return FALSE;
}

/*
 * One capture.  With @cancel_after_ms > 0 the capture is cancelled instead of
 * waited on; no finger is needed then, and none is requested.
 */
static void
capture_once (FpDevice *device, guint cancel_after_ms)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  g_autoptr (FpImage) image = NULL;
  guint guard;

  if (cancel_after_ms > 0)
    {
      /* No press is wanted here: this capture is cancelled shortly. */
      press_wanted = FALSE;
      printf ("capture: starting, will cancel after %u ms\n", cancel_after_ms);
      g_timeout_add (cancel_after_ms, cancel_cb, cancellable);
    }
  else
    {
      /* SPEC §4.5 操作前提: a press is wanted, and the cue is emitted the moment
       * the device is armed (see on_finger_status_changed). */
      press_wanted = TRUE;
      printf ("capture: waiting for the finger (driver window 30000 ms)\n"
              "         press as soon as the cue appears, then hold it for >= 10 s\n");
    }

  guard = g_timeout_add (CAPTURE_GUARD_MS, cancel_cb, cancellable);
  image = fp_device_capture_sync (device, TRUE, cancellable, &error);
  g_source_remove (guard);

  if (image != NULL)
    {
      printf ("capture: ok\n");
      describe_image (image);
    }
  else
    {
      print_error ("capture", error);
    }

  press_wanted = FALSE;
}

/* SPEC §4.5 操作前提: the host must give a perceptible cue at the moment ARM goes
 * out, and the operator presses only afterwards and holds.  The finger-status
 * signal reaches the host exactly at that transition, so the cue belongs there;
 * a desktop notification works even when this tool runs from another terminal,
 * and the bell covers environments without a notification daemon. */

static void
cue_operator (void)
{
  gchar *notify;

  fputs ("\a\n*** ARM SENT -- PRESS THE SENSOR NOW AND HOLD IT (>= 10 s) ***\n", stdout);
  fflush (stdout);

  notify = g_strdup_printf ("notify-send -u critical -t 0 '%s' '%s'", "FPC Disum: ARM sent",
      "Press the sensor now and hold it (>= 10 s)");
  g_spawn_command_line_async (notify, NULL);
  g_free (notify);
  g_spawn_command_line_async ("canberra-gtk-play -i bell", NULL);
}

static void
on_finger_status_changed (GObject *object, GParamSpec *pspec)
{
  FpDevice *device = FP_DEVICE (object);

  if (press_wanted && (fp_device_get_finger_status (device) & FP_FINGER_STATUS_NEEDED) != 0)
    cue_operator ();
}

int
main (int argc, char **argv)
{
  const gchar *command;
  guint cancel_after_ms = 3000;
  g_autoptr (FpContext) context = NULL;
  g_autoptr (FpDevice) device = NULL;
  int result = 1;

  g_set_prgname ("fpc-disum-hw-probe");

  if (argc < 2)
    {
      fprintf (stderr,
          "usage: %s <list|negative|capture|cancel|cancel-reopen|reopen>"
          " [--cancel-after-ms N]\n",
          argv[0]);
      return 2;
    }

  command = argv[1];
  for (gint i = 2; i < argc; i++)
    {
      if (g_strcmp0 (argv[i], "--cancel-after-ms") == 0 && i + 1 < argc)
        cancel_after_ms = (guint)g_ascii_strtoull (argv[++i], NULL, 10);
      else
        {
          fprintf (stderr, "unknown option: %s\n", argv[i]);
          return 2;
        }
    }

  /* The context, not the device, owns the libusb context: keep it for the run. */
  context = fp_context_new ();
  device = find_device (context);

  g_signal_connect (device, "notify::finger-status", G_CALLBACK (on_finger_status_changed), NULL);
  if (device == NULL)
    return 1;

  if (g_strcmp0 (command, "list") == 0)
    return 0;

  if (g_strcmp0 (command, "negative") == 0)
    {
      if (!open_device (device))
        return 1;

      /* Expect the driver's own touch window to expire (SPEC §16.3). */
      capture_once (device, 0);

      return close_device (device) ? 0 : 1;
    }

  if (g_strcmp0 (command, "capture") == 0 || g_strcmp0 (command, "cancel") == 0)
    {
      guint cancel = g_strcmp0 (command, "cancel") == 0 ? cancel_after_ms : 0;

      if (!open_device (device))
        return 1;

      capture_once (device, cancel);

      return close_device (device) ? 0 : 1;
    }

  if (g_strcmp0 (command, "cancel-reopen") == 0)
    {
      /* Cancel a running capture, then run a fresh session (SPEC §16.3). */
      if (!open_device (device))
        return 1;
      capture_once (device, cancel_after_ms);
      /* The cancelled session is *supposed* to fail, and its close carries that
       * first cause (the driver keeps it deliberately), so it must not stop the
       * sequence -- the second session is what this mode is about. */
      if (!close_device (device))
        printf ("note: the close carried the cancelled session's first cause\n");

      printf ("-- second session after the cancellation --\n");
      if (!open_device (device))
        return 1;
      capture_once (device, 0);
      result = close_device (device) ? 0 : 1;

      return result;
    }

  if (g_strcmp0 (command, "reopen") == 0)
    {
      /* Two sessions in one process; two images must differ (SPEC §16.4). */
      if (!open_device (device))
        return 1;
      capture_once (device, 0);
      if (!close_device (device))
        printf ("note: the first close carried the first session's cause\n");

      printf ("-- second session --\n");
      if (!open_device (device))
        return 1;
      capture_once (device, 0);
      result = close_device (device) ? 0 : 1;

      return result;
    }

  fprintf (stderr, "unknown command: %s\n", command);

  return 2;
}
