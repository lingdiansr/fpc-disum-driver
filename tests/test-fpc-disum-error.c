/*
 * FPC Disum (10a5:9201) session outcome to libfprint error mapping tests.
 *
 * Reference: docs/CLEANROOM_SPEC.md §16.2 and §16.3: cancellation, an expired
 * window, a USB failure and a protocol failure stay distinguishable.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <glib.h>

#include "fpc_disum_error.h"
#include "fp-device.h"

static void
test_status_mapping (void)
{
  g_autoptr (GError) error = NULL;

  /* A successful session reports nothing. */
  g_assert_null (fpc_disum_error_from_status (FPC_DISUM_SESSION_OK, NULL));

  /* Cancellation is its own outcome, not a device failure (SPEC §16.3). */
  error = fpc_disum_error_from_status (FPC_DISUM_SESSION_ERROR_CANCELLED, NULL);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);

  /* A window that expired is a timeout, for both windows (SPEC §16.3). */
  error = fpc_disum_error_from_status (FPC_DISUM_SESSION_ERROR_NO_TOUCH, NULL);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&error);

  error = fpc_disum_error_from_status (FPC_DISUM_SESSION_ERROR_NO_IMAGE, NULL);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&error);

  /* A USB failure is reported as such (SPEC §16.2). */
  error = fpc_disum_error_from_status (FPC_DISUM_SESSION_ERROR_IO, NULL);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Protocol and security failures are protocol errors. */
  {
    static const FpcDisumSessionStatus protocol[] = {
      FPC_DISUM_SESSION_ERROR_PROTOCOL,
      FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT,
      FPC_DISUM_SESSION_ERROR_ENVELOPE,
      FPC_DISUM_SESSION_ERROR_TLS,
      FPC_DISUM_SESSION_ERROR_ARM_RESULT,
    };

    for (guint i = 0; i < G_N_ELEMENTS (protocol); i++)
      {
        error = fpc_disum_error_from_status (protocol[i], NULL);
        g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
        g_clear_error (&error);
      }
  }

  /* A programming error is not dressed up as a device result. */
  error = fpc_disum_error_from_status (FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT, NULL);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
}

/* The session's own message becomes the error text. */
static void
test_message_is_used (void)
{
  g_autoptr (GError) error = fpc_disum_error_from_status (
      FPC_DISUM_SESSION_ERROR_NO_TOUCH, "no touch event arrived within the window");

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_cmpstr (error->message, ==, "no touch event arrived within the window");
}

/* No failing outcome is reported without text, and the text never names a key. */
static void
test_every_failure_has_a_message (void)
{
  for (gint status = FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT;
      status <= FPC_DISUM_SESSION_ERROR_CANCELLED; status++)
    {
      g_autoptr (GError) error = fpc_disum_error_from_status ((FpcDisumSessionStatus)status, NULL);

      g_assert_nonnull (error);
      g_assert_nonnull (error->message);
      g_assert_cmpuint (error->message[0], !=, 0);
      g_assert_cmpuint (g_utf8_validate (error->message, -1, NULL), ==, TRUE);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/error/status-mapping", test_status_mapping);
  g_test_add_func ("/error/message-is-used", test_message_is_used);
  g_test_add_func ("/error/every-failure-has-a-message", test_every_failure_has_a_message);

  return g_test_run ();
}
