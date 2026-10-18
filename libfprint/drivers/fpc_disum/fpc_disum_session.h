/*
 * FPC Disum (10a5:9201) single-capture session.
 *
 * Reference: docs/CLEANROOM_SPEC.md §6 (initialisation), §8 (security channel),
 * §4.5 (single capture), §16 (failure, cancellation and cleanup).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "fpc_disum_transport.h"

G_BEGIN_DECLS

/* SPEC §16.2 suggests failing a handshake that makes no progress for 5 s. */
#define FPC_DISUM_HANDSHAKE_TIMEOUT_MS 5000
/* SPEC §6 step 4 gives the device up to 20 s to become visible again. */
#define FPC_DISUM_REOPEN_TIMEOUT_MS 20000
/* SPEC §16.3: default window for waiting for the touch, used for the image too. */
#define FPC_DISUM_CAPTURE_WINDOW_MS 30000

typedef enum
{
  FPC_DISUM_SESSION_OK = 0,
  /* Programming error (NULL argument or wrong call order); never a device result. */
  FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT,
  /* Control or bulk transfer failed, the device disappeared, or it never came back
   * after the CMD_INIT handle swap (SPEC §6, §16.2). */
  FPC_DISUM_SESSION_ERROR_IO,
  /* Malformed frame or an impossible application message (SPEC §4.4, §16.1). */
  FPC_DISUM_SESSION_ERROR_PROTOCOL,
  /* An event code outside the reviewed event table arrived (SPEC §16.1). */
  FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT,
  /* The GET_TLS_KEY envelope was rejected (SPEC §8, §16.2). */
  FPC_DISUM_SESSION_ERROR_ENVELOPE,
  /* PSK identity, alert, Finished/MAC or a handshake that made no progress
   * (SPEC §8, §16.2). */
  FPC_DISUM_SESSION_ERROR_TLS,
  /* No EVT_FINGER_DWN within the window; a fixed delay is never a substitute and
   * GET_IMG is not sent (SPEC §16.1/§16.3). */
  FPC_DISUM_SESSION_ERROR_NO_TOUCH,
  /* A touch was seen but no image arrived within the window (SPEC §16.3). */
  FPC_DISUM_SESSION_ERROR_NO_IMAGE,
  /* EVT_ARM_RESULT carried the reserved third-field value 0x64; the value is
   * preserved but never named (SPEC §16.1). */
  FPC_DISUM_SESSION_ERROR_ARM_RESULT,
  /* Cancelled by the caller; not reported as a device failure (SPEC §16.3). */
  FPC_DISUM_SESSION_ERROR_CANCELLED,
} FpcDisumSessionStatus;

typedef struct _FpcDisumSession FpcDisumSession;

/* One event frame as it arrived, kept for diagnostics (SPEC §16.1). */
typedef struct
{
  guint32 code;
  guint32 total_len;
  guint32 third_field;
} FpcDisumSessionEvent;

typedef struct
{
  FpcDisumSessionStatus status;
  /* Static, never contains key material. */
  const gchar *message;
  /* Pixel plane, borrowed from the session and valid until the next call. */
  const guint8 *image;
  gsize image_len;
} FpcDisumCaptureResult;

typedef void (*FpcDisumSessionDoneFunc) (
    FpcDisumSessionStatus status, const gchar *message, gpointer user_data);
typedef void (*FpcDisumSessionCaptureFunc) (
    const FpcDisumCaptureResult *result, gpointer user_data);

/*
 * Timeout seam.  The default is a GLib main-loop timeout; the offline tests install
 * a controllable source so a window can expire without waiting for it.  @context is
 * passed back to both callbacks, and @add returns a handle removed by @remove.
 */
typedef struct
{
  gpointer (*add) (gpointer context, guint interval_ms, GSourceFunc callback, gpointer user_data);
  void (*remove) (gpointer context, gpointer handle);
  gpointer context;
} FpcDisumTimeoutSource;

/*
 * A session drives one device session: §6 initialisation followed by the §8
 * handshake, one §4.5 capture and the §16.3 cleanup.  It owns the TLS channel and
 * the PSK; the transport belongs to the caller and must outlive the session.
 *
 * Nothing here blocks: every step completes through a callback, so the session runs
 * on the caller's main loop and needs no thread of its own.
 */
FpcDisumSession *fpc_disum_session_new (FpcDisumTransport *transport);
/*
 * Release the session.  The caller must have completed close_async() first; the
 * transport is not touched and stays owned by the caller.
 */
void fpc_disum_session_free (FpcDisumSession *session);

/*
 * Replace the timeout source.  A NULL @source restores the default.  Only allowed
 * before the first operation is started.
 */
void fpc_disum_session_set_timeout_source (
    FpcDisumSession *session, const FpcDisumTimeoutSource *source);
/*
 * Override a window; pass 0 to use the default.  Only allowed before the first
 * operation is started, and the values are never interpreted as "wait forever".
 */
void fpc_disum_session_set_timeouts (
    FpcDisumSession *session, guint handshake_ms, guint reopen_ms, guint capture_window_ms);

/*
 * Called once while a capture waits, when the device reports the touch that makes
 * the session send GET_IMG.  The image-device layer uses it to report the finger
 * as present at the moment the device saw it; a second touch in the same capture
 * is not reported again.
 */
typedef void (*FpcDisumTouchFunc) (gpointer user_data);
void fpc_disum_session_set_touch_handler (
    FpcDisumSession *session, FpcDisumTouchFunc handler, gpointer user_data);

/*
 * SPEC §6 + §8: 0x08, GET_STATE, CMD_INIT, the handle swap, GET_TLS_KEY with
 * envelope authentication, TLS_INIT and the PSK handshake.  On success the session
 * holds an established channel; EVT_INIT_RESULT is recorded but never gates this.
 */
void fpc_disum_session_init_async (
    FpcDisumSession *session, FpcDisumSessionDoneFunc done, gpointer user_data);

/*
 * SPEC §4.5: ARM, wait for EVT_FINGER_DWN (GET_IMG is never sent before it), then
 * GET_IMG and assemble the image from the decrypted application stream.  The number
 * of EVT_TLS frames carrying the image is not assumed.
 */
void fpc_disum_session_capture_async (
    FpcDisumSession *session, FpcDisumSessionCaptureFunc done, gpointer user_data);

/*
 * SPEC §16.3: stop reading, then ARM_STOP (only if the session was armed), ABORT and
 * SESSION_OFF, releasing the TLS channel and the PSK.  Cleanup always runs to
 * completion; the report carries the first root cause, so a cleanup failure never
 * hides it.
 */
void fpc_disum_session_close_async (
    FpcDisumSession *session, FpcDisumSessionDoneFunc done, gpointer user_data);

/*
 * Stop advancing the current operation, which then reports
 * FPC_DISUM_SESSION_ERROR_CANCELLED exactly once.  Close_async() still performs the
 * full cleanup.  Safe to call more than once and from inside a callback.
 */
void fpc_disum_session_cancel (FpcDisumSession *session);
gboolean fpc_disum_session_is_cancelled (const FpcDisumSession *session);

/*
 * Frames the session received and did not act on, oldest first: the
 * reviewed-but-irrelevant ones and any unreviewed code (SPEC §16.1 "record and
 * ignore").  Frames the session consumed are not listed, and the record is
 * bounded, dropping its oldest entry once it is full.
 */
gsize fpc_disum_session_event_count (const FpcDisumSession *session);
const FpcDisumSessionEvent *fpc_disum_session_event (const FpcDisumSession *session, gsize index);

/* Whether ARM_STOP was sent for the session, i.e. whether it was armed. */
gboolean fpc_disum_session_was_armed (const FpcDisumSession *session);

G_END_DECLS
