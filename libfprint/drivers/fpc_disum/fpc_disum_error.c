/*
 * FPC Disum (10a5:9201) session outcome to libfprint error mapping.
 *
 * Reference: docs/CLEANROOM_SPEC.md §16.2 and §16.3.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_error.h"

/* The public error domain keeps this module linkable without the internal
 * library, which is what the native test uses. */
#include "fp-device.h"

static const gchar *
default_message (FpcDisumSessionStatus status)
{
  switch (status)
    {
    case FPC_DISUM_SESSION_OK:
      return NULL;
    case FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT:
      return "Invalid argument";
    case FPC_DISUM_SESSION_ERROR_IO:
      return "USB transfer failed";
    case FPC_DISUM_SESSION_ERROR_PROTOCOL:
      return "Protocol error";
    case FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT:
      return "Unexpected event";
    case FPC_DISUM_SESSION_ERROR_ENVELOPE:
      return "The key envelope was rejected";
    case FPC_DISUM_SESSION_ERROR_TLS:
      return "The security channel failed";
    case FPC_DISUM_SESSION_ERROR_NO_TOUCH:
      return "No touch event arrived within the window";
    case FPC_DISUM_SESSION_ERROR_NO_IMAGE:
      return "No image arrived within the window";
    case FPC_DISUM_SESSION_ERROR_ARM_RESULT:
      return "The arm result carries the reserved state value";
    case FPC_DISUM_SESSION_ERROR_CANCELLED:
      return "Cancelled";
    }

  return "Session failed";
}

GError *
fpc_disum_error_from_status (FpcDisumSessionStatus status, const gchar *message)
{
  const gchar *text = message != NULL ? message : default_message (status);

  switch (status)
    {
    case FPC_DISUM_SESSION_OK:
      return NULL;
    case FPC_DISUM_SESSION_ERROR_CANCELLED:
      return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, text);
    case FPC_DISUM_SESSION_ERROR_NO_TOUCH:
    case FPC_DISUM_SESSION_ERROR_NO_IMAGE:
      return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, text);
    case FPC_DISUM_SESSION_ERROR_IO:
      return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, text);
    case FPC_DISUM_SESSION_ERROR_PROTOCOL:
    case FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT:
    case FPC_DISUM_SESSION_ERROR_ENVELOPE:
    case FPC_DISUM_SESSION_ERROR_TLS:
    case FPC_DISUM_SESSION_ERROR_ARM_RESULT:
      return g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO, "%s", text);
    case FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT:
      return g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL, "%s", text);
    }

  return g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL, "%s", text);
}
