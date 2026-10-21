/*
 * FPC Disum enroll/verify probe.
 *
 * Exercises the same libfprint entry points fprintd uses:
 * fp_device_enroll_sync() on one open, then fp_device_verify_sync() on a
 * fresh open with the resulting in-memory FpPrint.  It prints no image,
 * digest, key or payload; the print is never written to disk.
 *
 * This is a manual probe, not a test and never registered as one.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>

#include "fp-context.h"
#include "fp-device.h"
#include "fp-print.h"

#define OPEN_GUARD_MS 60000
#define ACTION_GUARD_MS 180000

static guint enroll_delay_ms = 800;

static void
print_error (const gchar *what, const GError *error)
{
  if (error == NULL)
    printf ("%s: ok\n", what);
  else
    printf ("%s: error domain=%s code=%d message=\"%s\"\n", what, g_quark_to_string (error->domain),
        error->code, error->message);
}

static gboolean
cancel_cb (gpointer user_data)
{
  g_cancellable_cancel (G_CANCELLABLE (user_data));
  return G_SOURCE_REMOVE;
}

static FpDevice *
find_device (FpContext *context)
{
  GPtrArray *devices;
  FpDevice *found = NULL;

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
          if (found != NULL)
            {
              printf ("error: more than one fpc_disum device\n");
              g_object_unref (found);
              return NULL;
            }
          found = g_object_ref (device);
        }
    }

  if (found == NULL)
    printf ("error: no fpc_disum device was found\n");

  return found;
}

static void
cue_operator (void)
{
  const gchar *notify_argv[] = { "notify-send", "-u", "critical", "-t", "0", "FPC Disum: ARM sent",
    "Press the sensor now and hold it", NULL };
  const gchar *bell_argv[] = { "canberra-gtk-play", "-i", "bell", NULL };

  fputs ("*** ARM sent: press the sensor now and hold it ***\n", stdout);
  fflush (stdout);
  g_spawn_async (NULL, (gchar **)notify_argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
      NULL, NULL, NULL, NULL);
  g_spawn_async (NULL, (gchar **)bell_argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
      NULL, NULL, NULL, NULL);
}

static void
on_arm_complete (FpDevice *device, guint actual_length, gpointer user_data)
{
  (void)device;
  (void)user_data;
  if (actual_length == 4)
    cue_operator ();
}

static void
enroll_progress (
    FpDevice *device, gint completed_stages, FpPrint *print, gpointer user_data, GError *error)
{
  (void)device;
  (void)print;
  (void)user_data;

  printf ("enroll: stage %d/%d%s%s\n", completed_stages, fp_device_get_nr_enroll_stages (device),
      error != NULL ? " (error): " : "", error != NULL ? error->message : "");
  fflush (stdout);

  /* Probe-only operator pacing: the next ARM follows immediately otherwise. */
  if (error == NULL && completed_stages < fp_device_get_nr_enroll_stages (device)
      && enroll_delay_ms > 0)
    g_usleep ((gulong)enroll_delay_ms * 1000);
}

static gboolean
open_device (FpDevice *device, GCancellable *cancellable)
{
  g_autoptr (GError) error = NULL;
  guint guard = g_timeout_add (OPEN_GUARD_MS, cancel_cb, cancellable);

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

static void
close_device (FpDevice *device)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;

  if (!fp_device_close_sync (device, cancellable, &error))
    print_error ("close", error);
  else
    printf ("close: ok\n");
}

static void
describe_features (FpDevice *device)
{
  FpDeviceFeature features = fp_device_get_features (device);

  printf ("features: capture=%s verify=%s identify=%s storage=%s\n",
      (features & FP_DEVICE_FEATURE_CAPTURE) ? "yes" : "no",
      (features & FP_DEVICE_FEATURE_VERIFY) ? "yes" : "no",
      (features & FP_DEVICE_FEATURE_IDENTIFY) ? "yes" : "no",
      (features & FP_DEVICE_FEATURE_STORAGE) ? "yes" : "no");
  printf ("enroll stages: %d\n", fp_device_get_nr_enroll_stages (device));
}

int
main (int argc, char **argv)
{
  g_autoptr (FpContext) context = NULL;
  g_autoptr (FpDevice) device = NULL;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  g_autoptr (FpPrint) print = NULL;
  gboolean match = FALSE;
  gboolean enroll_ok = FALSE;
  gboolean verify_ok = FALSE;
  gboolean any_match = FALSE;
  guint verify_attempts = 3;
  guint action_guard;
  guint arm_handler = 0;

  for (int i = 1; i < argc; i++)
    {
      if (g_strcmp0 (argv[i], "--verify-attempts") == 0 && i + 1 < argc)
        {
          verify_attempts = (guint)g_ascii_strtoull (argv[++i], NULL, 10);
          if (verify_attempts == 0)
            verify_attempts = 1;
        }
    }

  context = fp_context_new ();
  device = find_device (context);
  if (device == NULL)
    return 1;

  describe_features (device);

  if (g_signal_lookup ("arm-complete", G_OBJECT_TYPE (device)) != 0)
    arm_handler = g_signal_connect (device, "arm-complete", G_CALLBACK (on_arm_complete), NULL);
  else
    printf ("note: driver has no arm-complete signal; no live cue\n");

  if (!open_device (device, cancellable))
    return 1;

  print = fp_print_new (device);
  action_guard = g_timeout_add (ACTION_GUARD_MS, cancel_cb, cancellable);
  print = fp_device_enroll_sync (
      device, g_steal_pointer (&print), cancellable, enroll_progress, NULL, &error);
  g_source_remove (action_guard);
  enroll_ok = print != NULL;
  print_error ("enroll", error);
  g_clear_error (&error);

  close_device (device);
  if (!enroll_ok)
    return 1;

  if (!open_device (device, cancellable))
    {
      g_clear_object (&print);
      return 1;
    }

  for (guint attempt = 1; attempt <= verify_attempts; attempt++)
    {
      match = FALSE;
      action_guard = g_timeout_add (ACTION_GUARD_MS, cancel_cb, cancellable);
      verify_ok
          = fp_device_verify_sync (device, print, cancellable, NULL, NULL, &match, NULL, &error);
      g_source_remove (action_guard);

      if (!verify_ok)
        {
          print_error ("verify", error);
          g_clear_error (&error);
          break;
        }

      printf (
          "verify attempt %u/%u: match=%s\n", attempt, verify_attempts, match ? "true" : "false");
      fflush (stdout);
      if (match)
        any_match = TRUE;
    }

  close_device (device);

  if (arm_handler != 0)
    g_signal_handler_disconnect (device, arm_handler);

  return (enroll_ok && verify_ok && any_match) ? 0 : 1;
}
