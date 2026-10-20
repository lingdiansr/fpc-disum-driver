/*
 * FPC Disum (10a5:9201) session outcome to libfprint error mapping.
 *
 * Reference: docs/CLEANROOM_SPEC.md §16.2 (TLS, authentication and USB errors)
 * and §16.3 (cancellation, timeout and cleanup).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "fpc_disum_session.h"

G_BEGIN_DECLS

/*
 * Build the error an image-device action reports for a session outcome.  The five
 * cases the contract keeps apart stay apart:
 *
 *   - cancellation          -> G_IO_ERROR_CANCELLED
 *   - a window that expired -> G_IO_ERROR_TIMED_OUT
 *   - a USB failure or a connection-generation change -> G_IO_ERROR_FAILED
 *   - an unsupported automatic reset recovery -> FP_DEVICE_ERROR_NOT_SUPPORTED
 *     with the stable logical string "unsupported-reset-recovery"
 *   - protocol or security  -> FP_DEVICE_ERROR_PROTO
 *   - a programming error   -> FP_DEVICE_ERROR_GENERAL
 *
 * FPC_DISUM_SESSION_OK returns NULL.  @message, when not NULL, becomes the error
 * text and never contains key material.
 */
GError *fpc_disum_error_from_status (FpcDisumSessionStatus status, const gchar *message);

G_END_DECLS
