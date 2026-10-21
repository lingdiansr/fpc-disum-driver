/*
 * FPC Disum (10a5:9201) single-capture session.
 *
 * Reference: docs/CLEANROOM_SPEC.md §6 (initialisation), §8 (security channel),
 * §4.5 (single capture), §16 (failure, cancellation and cleanup).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_session.h"

#include <string.h>

#include "fpc_disum_diagnostics.h"
#include "fpc_disum_proto.h"
#include "fpc_disum_tls.h"

/* Command ids (SPEC §4.1). */
#define CMD_INIT 0x01
#define CMD_ARM 0x02
#define CMD_ABORT 0x03
#define CMD_TLS_INIT 0x05
#define CMD_TLS_DATA 0x06
#define CMD_INDICATE_S_STATE 0x08
#define CMD_GET_IMG 0x09
#define CMD_GET_TLS_KEY 0x0b
#define CMD_SESSION_OFF 0x12
#define CMD_GET_STATE 0x50

/* Event ids (SPEC §4.2). */
#define EVT_HELLO 0x01
#define EVT_INIT_RESULT 0x02
#define EVT_ARM_RESULT 0x03
#define EVT_DEAD_PIXEL_REPORT 0x04
#define EVT_TLS 0x05
#define EVT_FINGER_DWN 0x06
#define EVT_FINGER_UP 0x07
#define EVT_USB_LOGS 0x09
#define EVT_TLS_KEY 0x0a
#define EVT_REFRESH_SENSOR 0x20

/* Command parameters (SPEC §3.1, §4.1). */
#define INDICATE_S_STATE_VALUE 0x0010
#define INIT_VALUE 0x0001
#define INIT_PAYLOAD_LEN 4
#define TLS_INIT_VALUE 0x0001
#define ARM_MODE_VERIFY 1
#define ARM_WLENGTH 4
#define GET_STATE_CAPACITY 72
#define GET_TLS_KEY_CAPACITY 1000

/* The reviewed arm payload and its cleanup counterpart (SPEC §4.1, §16.3). */
static const guint8 arm_payload[ARM_WLENGTH] = { 0x11, 0x2f, 0x11, 0x17 };
static const guint8 arm_stop_payload[ARM_WLENGTH] = { 0x12, 0x2f, 0x11, 0x17 };

/* Whether the arm result carries the reserved state value (SPEC §16.1). */
#define ARM_RESULT_RESERVED 0x64

/* Bounded event record; the log is a diagnostic aid, not a capture buffer. */
#define EVENT_LOG_MAX 64
#define APPLICATION_CHUNK 4096

/* SPEC §16.3 cleanup sequence: ARM_STOP (only when it may be needed), ABORT,
 * SESSION_OFF.  Every entry is attempted in order and recorded separately. */
#define CLEANUP_ARM_STOP 0
#define CLEANUP_ABORT 1
#define CLEANUP_SESSION_OFF 2
#define CLEANUP_COUNT 3

/* A peer operation is one control transfer, bulk read or reopen. */
typedef enum
{
  ST_NONE = 0,
  /* SPEC §6, in order. */
  ST_INIT_INDICATE_S_STATE,
  ST_INIT_GET_STATE,
  ST_INIT_CMD_INIT,
  ST_INIT_REOPEN,
  ST_INIT_GET_TLS_KEY,
  ST_INIT_TLS_INIT,
  ST_HANDSHAKE,
  /* SPEC §4.5. */
  ST_CAPTURE_ARM,
  ST_CAPTURE_WAIT_TOUCH,
  ST_CAPTURE_GET_IMG,
  ST_CAPTURE_IMAGE,
  /* SPEC §16.3. */
  ST_CLOSE_ARM_STOP,
  ST_CLOSE_ABORT,
  ST_CLOSE_SESSION_OFF,
  /* A terminal outcome is recorded and the in-flight I/O is being abandoned. */
  ST_QUIESCE,
  ST_DONE,
} SessionState;

typedef enum
{
  OP_NONE = 0,
  OP_INIT,
  OP_CAPTURE,
  OP_CLOSE,
} OperationKind;

typedef struct
{
  gboolean attempted;
  gboolean submitted;
  FpcDisumTransportStatus status;
  gsize requested_length;
  gboolean actual_known;
  gsize actual_length;
  /* Static record disposition when the command was not sent. */
  const gchar *result;
} CleanupRecord;

struct _FpcDisumSession
{
  FpcDisumTransport *transport;
  FpcDisumTimeoutSource timeouts;
  FpcDisumDiagnostics *diagnostics;

  guint handshake_ms;
  guint reopen_ms;
  guint window_ms;

  SessionState state;
  OperationKind operation;
  gboolean stepping;
  gboolean step_again;
  gboolean cancelled;
  gboolean in_close;
  gboolean armed;
  /* Whether an ARM_STOP was put on the wire during cleanup. */
  gboolean arm_stop_sent;
  /* ARM was submitted but its acceptance can no longer be proven. */
  gboolean arm_maybe_accepted;
  gboolean arm_submitted;
  /* A cancelled operation may have reached the device; only cancel can clear it. */
  gboolean cancel_requested;

  /* Exactly one transport operation is outstanding at a time. */
  guint pending_ops;
  gboolean terminal_pending;
  gboolean abnormal_reset_recorded;
  gboolean connection_dead;

  /* The length of the outstanding control OUT; success needs the same length. */
  gsize out_requested_length;

  /* CMD_INIT reclaim transaction and the absolute 20 s deadline from ALLOW. */
  gboolean rebind_open;
  gboolean rebind_allowed;
  gboolean rebind_committed;
  gint64 reopen_deadline_us;
  gboolean key_retry_used;

  /* Generation observed when the security channel started. */
  gboolean tls_started;
  guint64 tls_generation;
  guint64 generation;

  gsize discarded_length;
  CleanupRecord cleanup[CLEANUP_COUNT];
  guint cleanup_index;
  gboolean cleanup_failed;

  /* First root cause; later failures are recorded but never replace it. */
  FpcDisumSessionStatus failure;
  const gchar *failure_message;

  FpcDisumSessionDoneFunc done;
  gpointer done_data;
  FpcDisumSessionCaptureFunc capture_done;

  FpcDisumTouchFunc touch_handler;
  gpointer touch_data;
  FpcDisumArmCompleteFunc arm_complete_handler;
  gpointer arm_complete_data;

  gpointer timeout_handle;

  FpcDisumTls *tls;
  FpcDisumPsk psk;
  FpcDisumParser events;
  FpcDisumParser application;
  GByteArray *application_message;
  const guint8 *image;
  gsize image_len;

  FpcDisumSessionEvent event_log[EVENT_LOG_MAX];
  guint n_events;

  FpcDisumDiagnosticPhase phase;

  guint8 tls_chunk[FPC_DISUM_TLS_DATA_MAX];
  guint8 application_chunk[APPLICATION_CHUNK];
};

/* ------------------------------------------------------------ diagnostics */

static const gchar *
logical_result_name (FpcDisumSessionStatus status)
{
  switch (status)
    {
    case FPC_DISUM_SESSION_OK:
      return "ok";
    case FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT:
      return "invalid-argument";
    case FPC_DISUM_SESSION_ERROR_IO:
      return "io";
    case FPC_DISUM_SESSION_ERROR_PROTOCOL:
      return "protocol";
    case FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT:
      return "protocol";
    case FPC_DISUM_SESSION_ERROR_ENVELOPE:
      return "envelope";
    case FPC_DISUM_SESSION_ERROR_TLS:
      return "tls";
    case FPC_DISUM_SESSION_ERROR_NO_TOUCH:
      return "no-touch";
    case FPC_DISUM_SESSION_ERROR_NO_IMAGE:
      return "no-image";
    case FPC_DISUM_SESSION_ERROR_ARM_RESULT:
      return "arm-result";
    case FPC_DISUM_SESSION_ERROR_CANCELLED:
      return "cancelled";
    case FPC_DISUM_SESSION_ERROR_CONNECTION_CHANGED:
      return "connection-changed";
    case FPC_DISUM_SESSION_ERROR_UNSUPPORTED_RESET_RECOVERY:
      return "unsupported-reset-recovery";
    }

  return "";
}

static FpcDisumDiagnosticPhase
phase_for_state (SessionState state)
{
  switch (state)
    {
    case ST_NONE:
      return FPC_DISUM_DIAGNOSTIC_PHASE_NONE;
    case ST_INIT_INDICATE_S_STATE:
    case ST_INIT_GET_STATE:
      return FPC_DISUM_DIAGNOSTIC_PHASE_INIT;
    case ST_INIT_CMD_INIT:
    case ST_INIT_REOPEN:
      return FPC_DISUM_DIAGNOSTIC_PHASE_REBIND;
    case ST_INIT_GET_TLS_KEY:
      return FPC_DISUM_DIAGNOSTIC_PHASE_INIT;
    case ST_INIT_TLS_INIT:
    case ST_HANDSHAKE:
      return FPC_DISUM_DIAGNOSTIC_PHASE_HANDSHAKE;
    case ST_CAPTURE_ARM:
    case ST_CAPTURE_WAIT_TOUCH:
      return FPC_DISUM_DIAGNOSTIC_PHASE_WAIT_TOUCH;
    case ST_CAPTURE_GET_IMG:
    case ST_CAPTURE_IMAGE:
      return FPC_DISUM_DIAGNOSTIC_PHASE_IMAGE;
    case ST_CLOSE_ARM_STOP:
    case ST_CLOSE_ABORT:
    case ST_CLOSE_SESSION_OFF:
      return FPC_DISUM_DIAGNOSTIC_PHASE_CLEANUP;
    case ST_QUIESCE:
      return FPC_DISUM_DIAGNOSTIC_PHASE_QUIESCING;
    case ST_DONE:
      return FPC_DISUM_DIAGNOSTIC_PHASE_DONE;
    }

  return FPC_DISUM_DIAGNOSTIC_PHASE_NONE;
}

static void
update_phase (FpcDisumSession *self)
{
  FpcDisumDiagnosticPhase phase = phase_for_state (self->state);

  if (phase == self->phase)
    return;

  self->phase = phase;
  if (self->diagnostics != NULL)
    fpc_disum_diagnostics_set_phase (self->diagnostics, phase);
}

static void
emit_record (FpcDisumSession *self, const FpcDisumDiagnosticRecord *record)
{
  if (self->diagnostics == NULL)
    return;

  fpc_disum_diagnostics_emit (self->diagnostics, record);
}

/* ------------------------------------------------------------ timeouts */

static gpointer
glib_timeout_add (gpointer context, guint interval_ms, GSourceFunc callback, gpointer user_data)
{
  return GUINT_TO_POINTER (g_timeout_add (interval_ms, callback, user_data));
}

static void
glib_timeout_remove (gpointer context, gpointer handle)
{
  g_source_remove (GPOINTER_TO_UINT (handle));
}

static const FpcDisumTimeoutSource glib_timeouts = {
  .add = glib_timeout_add,
  .remove = glib_timeout_remove,
  .context = NULL,
};

static void
cancel_timer (FpcDisumSession *self)
{
  if (self->timeout_handle == NULL)
    return;

  self->timeouts.remove (self->timeouts.context, self->timeout_handle);
  self->timeout_handle = NULL;
}

static gboolean on_timeout (gpointer user_data);

static void
arm_timer (FpcDisumSession *self, guint interval_ms)
{
  cancel_timer (self);
  self->timeout_handle = self->timeouts.add (self->timeouts.context, interval_ms, on_timeout, self);
}

/* Arm the window that ends at @deadline_us (CLOCK_MONOTONIC). */
static void
arm_timer_until (FpcDisumSession *self, gint64 deadline_us)
{
  gint64 remaining = deadline_us - g_get_monotonic_time ();
  guint interval_ms;

  if (remaining <= 0)
    {
      arm_timer (self, 1);
      return;
    }

  interval_ms = (guint)MIN ((remaining + 999) / 1000, G_MAXUINT);
  arm_timer (self, MAX (interval_ms, 1));
}

/* ------------------------------------------------------------ bookkeeping */

static void
record_failure (FpcDisumSession *self, FpcDisumSessionStatus status, const gchar *message)
{
  if (status == FPC_DISUM_SESSION_OK || self->failure != FPC_DISUM_SESSION_OK)
    return;

  self->failure = status;
  self->failure_message = message;
}

/* Record a frame the session did not act on (SPEC §16.1). */
static void
record_event (FpcDisumSession *self, const FpcDisumFrame *frame)
{
  guint slot;

  if (self->n_events == EVENT_LOG_MAX)
    {
      memmove (
          self->event_log, self->event_log + 1, (EVENT_LOG_MAX - 1) * sizeof self->event_log[0]);
      slot = EVENT_LOG_MAX - 1;
    }
  else
    {
      slot = self->n_events++;
    }

  self->event_log[slot].code = frame->code;
  self->event_log[slot].total_len = frame->total_len;
  self->event_log[slot].third_field = frame->third_field;
}

static void
release_channel (FpcDisumSession *self)
{
  g_clear_pointer (&self->tls, fpc_disum_tls_free);
  fpc_disum_psk_clear (&self->psk);
  self->image = NULL;
  self->image_len = 0;
}

static FpcDisumTransportIdentity
current_identity (FpcDisumSession *self)
{
  FpcDisumTransportIdentity identity = { 0 };

  if (self->transport->identity != NULL)
    self->transport->identity (self->transport, &identity);

  return identity;
}

static void on_event_frame (const FpcDisumFrame *frame, gpointer user_data);
static void on_application_frame (const FpcDisumFrame *frame, gpointer user_data);

/* A real generation change discards every partial of the old stream; it is
 * never concatenated with the new one (SPEC §6). */
static void
note_generation (FpcDisumSession *self)
{
  FpcDisumTransportIdentity identity = current_identity (self);

  if (identity.generation == 0)
    return;

  if (self->generation != 0 && self->generation != identity.generation)
    {
      gsize discarded = fpc_disum_parser_pending (&self->events)
                        + fpc_disum_parser_pending (&self->application);
      FpcDisumDiagnosticRecord record = { 0 };

      fpc_disum_parser_clear (&self->events);
      fpc_disum_parser_init (&self->events, FPC_DISUM_STREAM_EVENT, on_event_frame, self);
      fpc_disum_parser_clear (&self->application);
      fpc_disum_parser_init (
          &self->application, FPC_DISUM_STREAM_APPLICATION, on_application_frame, self);
      g_clear_pointer (&self->tls, fpc_disum_tls_free);
      fpc_disum_psk_clear (&self->psk);
      self->discarded_length += discarded;

      record.kind = FPC_DISUM_DIAGNOSTIC_KIND_IDENTITY;
      record.discarded_valid = TRUE;
      record.discarded_length = discarded;
      emit_record (self, &record);
    }

  self->generation = identity.generation;
}

/* The initialisation transaction must be closed exactly once. */
static void
abort_rebind (FpcDisumSession *self)
{
  if (!self->rebind_open)
    return;

  self->rebind_open = FALSE;
  self->rebind_allowed = FALSE;

  if (self->transport->init_rebind != NULL)
    (void)self->transport->init_rebind (self->transport, FPC_DISUM_INIT_REBIND_ABORT, 0);
}

/* --------------------------------------------------------------- stepping */

static void session_step (FpcDisumSession *self);
static void deliver_terminal (FpcDisumSession *self);

static const gchar *
drop_reason (FpcDisumTransportStatus status)
{
  switch (status)
    {
    case FPC_DISUM_TRANSPORT_ERROR_CANCELLED:
      return "cancelled";
    case FPC_DISUM_TRANSPORT_TIMEOUT:
      return "the USB transfer timed out";
    case FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE:
      return "the device left the bus";
    case FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN:
      return "the transfer completion could not be trusted";
    case FPC_DISUM_TRANSPORT_ERROR_IO:
    case FPC_DISUM_TRANSPORT_OK:
      return "USB transfer failed";
    }

  return "USB transfer failed";
}

/*
 * Record the first root cause and stop the transaction.  If I/O is still in
 * flight the session goes quiet first: the completion is delivered only once the
 * outstanding operation has really finished, and never from inside a cancel
 * request.  The timeout/cancel completion that follows cannot replace the cause.
 */
static void
finish_operation (FpcDisumSession *self, FpcDisumSessionStatus status, const gchar *message)
{
  if (self->state == ST_DONE || self->terminal_pending)
    return;

  record_failure (self, status, message);
  cancel_timer (self);

  if (self->pending_ops > 0)
    {
      self->terminal_pending = TRUE;
      self->state = ST_QUIESCE;
      update_phase (self);

      if (!self->cancel_requested)
        {
          self->cancel_requested = TRUE;
          self->transport->cancel (self->transport);
        }

      return;
    }

  deliver_terminal (self);
}

/* Deliver the one completion of the operation, exactly once. */
static void
deliver_terminal (FpcDisumSession *self)
{
  OperationKind operation;
  FpcDisumSessionDoneFunc done;
  FpcDisumSessionCaptureFunc capture_done;
  gpointer user_data;
  FpcDisumDiagnosticRecord record = { 0 };

  if (self->state == ST_DONE)
    return;

  operation = self->operation;
  done = self->done;
  capture_done = self->capture_done;
  user_data = self->done_data;

  self->terminal_pending = FALSE;
  cancel_timer (self);
  self->state = ST_DONE;
  self->operation = OP_NONE;
  self->done = NULL;
  self->capture_done = NULL;
  self->done_data = NULL;

  if (operation == OP_INIT)
    abort_rebind (self);

  /* Whatever the parser still holds is abandoned, not reinterpreted. */
  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_ROUND_RESULT;
  record.primary_result = logical_result_name (self->failure);
  record.partial_valid = TRUE;
  record.partial_length = fpc_disum_parser_pending (&self->events);
  record.discarded_valid = TRUE;
  record.discarded_length = self->discarded_length + fpc_disum_parser_pending (&self->application);

  update_phase (self);
  emit_record (self, &record);

  if (operation == OP_CAPTURE && capture_done != NULL)
    {
      FpcDisumCaptureResult result = {
        .status = self->failure,
        .message = self->failure_message,
        .image = self->failure == FPC_DISUM_SESSION_OK ? self->image : NULL,
        .image_len = self->failure == FPC_DISUM_SESSION_OK ? self->image_len : 0,
      };

      capture_done (&result, user_data);
      return;
    }

  if (done != NULL)
    done (self->failure, self->failure_message, user_data);
}

static void
finish_close (FpcDisumSession *self, FpcDisumSessionStatus status, const gchar *message)
{
  FpcDisumSessionDoneFunc done;
  gpointer user_data;
  FpcDisumDiagnosticRecord record = { 0 };
  const gchar *cleanup_result;

  if (self->state == ST_DONE)
    return;

  done = self->done;
  user_data = self->done_data;

  self->state = ST_DONE;
  self->operation = OP_NONE;
  self->done = NULL;
  self->done_data = NULL;
  self->in_close = FALSE;

  cancel_timer (self);
  record_failure (self, status, message);
  abort_rebind (self);
  release_channel (self);

  if (self->connection_dead)
    cleanup_result = "unavailable";
  else if (self->cleanup_failed)
    cleanup_result = "io";
  else
    cleanup_result = "ok";

  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_ROUND_RESULT;
  record.primary_result = logical_result_name (self->failure);
  record.cleanup_result = cleanup_result;
  record.partial_valid = TRUE;
  record.partial_length = fpc_disum_parser_pending (&self->events);
  record.discarded_valid = TRUE;
  record.discarded_length = self->discarded_length + fpc_disum_parser_pending (&self->application);

  update_phase (self);
  emit_record (self, &record);

  if (done != NULL)
    done (self->failure, self->failure_message, user_data);
}

/*
 * A control or bulk completion was accepted.  Returns TRUE when the caller must
 * stop: the session is done, a terminal outcome took over, or the completion was
 * a cancellation.
 */
static gboolean
reject_if_cancelled (FpcDisumSession *self, FpcDisumTransportStatus status, gsize discarded)
{
  if (status != FPC_DISUM_TRANSPORT_ERROR_CANCELLED && !self->cancelled)
    return FALSE;

  self->discarded_length += discarded;
  if (self->state == ST_CAPTURE_ARM)
    {
      /* The device may have seen the command even though its completion was a
       * cancellation; only ARM_STOP can clear that (SPEC §16.3). */
      self->arm_maybe_accepted = TRUE;
    }
  finish_operation (self, FPC_DISUM_SESSION_ERROR_CANCELLED, drop_reason (status));
  return TRUE;
}

static gboolean
reject_if_failed (FpcDisumSession *self, FpcDisumTransportStatus status, gsize discarded)
{
  if (status == FPC_DISUM_TRANSPORT_OK || status == FPC_DISUM_TRANSPORT_TIMEOUT)
    return FALSE;

  self->discarded_length += discarded;

  if (status == FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE)
    {
      self->connection_dead = TRUE;
      if (self->operation == OP_CAPTURE)
        self->abnormal_reset_recorded = TRUE;
    }

  finish_operation (self, FPC_DISUM_SESSION_ERROR_IO, drop_reason (status));
  return TRUE;
}

/*
 * A generation change after TLS_INIT started ends the open; it is never hidden
 * by a transparent re-handshake or a new ARM (SPEC §6, §16.2).
 */
static gboolean
reject_if_connection_changed (FpcDisumSession *self, gsize discarded)
{
  FpcDisumTransportIdentity identity;

  if (!self->tls_started || self->in_close)
    return FALSE;

  identity = current_identity (self);
  if (identity.generation == 0 || self->tls_generation == 0
      || identity.generation == self->tls_generation)
    return FALSE;

  self->discarded_length += discarded;
  finish_operation (self, FPC_DISUM_SESSION_ERROR_CONNECTION_CHANGED,
      "The connection changed after the security channel started");
  return TRUE;
}

/* ------------------------------------------------------------- callbacks */

static gboolean
accept_control_out (FpcDisumSession *self, FpcDisumTransportStatus status, gsize actual_length)
{
  if (actual_length != self->out_requested_length)
    {
      /* A short OUT never advances the state machine (SPEC §16.2). */
      finish_operation (self, FPC_DISUM_SESSION_ERROR_IO, "The control transfer completed short");
      return FALSE;
    }

  return TRUE;
}

static void
cleanup_emit (FpcDisumSession *self, guint index)
{
  CleanupRecord *cleanup = &self->cleanup[index];
  FpcDisumDiagnosticRecord record = { 0 };

  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_CLEANUP;
  record.direction_valid = TRUE;
  record.direction = FPC_DISUM_DIAGNOSTIC_DIRECTION_OUT;
  record.control_valid = TRUE;
  record.bm_request_type = 0x40;
  record.request = index == CLEANUP_ARM_STOP ? CMD_ARM
                   : index == CLEANUP_ABORT  ? CMD_ABORT
                                             : CMD_SESSION_OFF;
  record.value = index == CLEANUP_ARM_STOP ? ARM_MODE_VERIFY : 0;
  record.index = 0;
  record.requested_length_valid = TRUE;
  record.requested_length = cleanup->requested_length;
  record.actual_length_known = cleanup->actual_known;
  record.actual_length = cleanup->actual_length;
  record.status_valid = TRUE;
  record.status = cleanup->status;
  record.submitted_valid = TRUE;
  record.submitted = cleanup->submitted;
  if (cleanup->result != NULL)
    record.cleanup_result = cleanup->result;
  else if (!cleanup->attempted)
    record.cleanup_result = "unavailable";
  else if (cleanup->status == FPC_DISUM_TRANSPORT_OK && cleanup->actual_known
           && cleanup->actual_length == cleanup->requested_length)
    record.cleanup_result = "ok";
  else
    record.cleanup_result = "io";

  emit_record (self, &record);
}

static void cleanup_advance (FpcDisumSession *self);

static void
cleanup_out_done (FpcDisumSession *self, FpcDisumTransportStatus status, gsize actual_length)
{
  CleanupRecord *cleanup;

  if (self->cleanup_index >= CLEANUP_COUNT)
    return;

  cleanup = &self->cleanup[self->cleanup_index];
  cleanup->attempted = TRUE;
  cleanup->submitted = TRUE;
  cleanup->status = status;
  cleanup->actual_known = TRUE;
  cleanup->actual_length = actual_length;

  if (status == FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE)
    {
      self->connection_dead = TRUE;
      self->cleanup_failed = TRUE;
      cleanup_emit (self, self->cleanup_index);

      /* Nothing more can be sent on a dead handle; the rest is unavailable. */
      for (guint i = self->cleanup_index + 1; i < CLEANUP_COUNT; i++)
        {
          self->cleanup[i].attempted = FALSE;
          self->cleanup[i].submitted = FALSE;
          self->cleanup[i].actual_known = FALSE;
          self->cleanup[i].status = FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE;
          cleanup_emit (self, i);
        }

      finish_close (self, FPC_DISUM_SESSION_ERROR_IO, "The device left the bus");
      return;
    }

  if (status != FPC_DISUM_TRANSPORT_OK || actual_length != cleanup->requested_length)
    self->cleanup_failed = TRUE;

  cleanup_emit (self, self->cleanup_index);

  if (self->cleanup_index == CLEANUP_ARM_STOP && status == FPC_DISUM_TRANSPORT_OK
      && actual_length == cleanup->requested_length)
    {
      self->armed = FALSE;
      self->arm_maybe_accepted = FALSE;
    }

  cleanup_advance (self);
}

static void
on_control_out (FpcDisumTransport *transport, FpcDisumTransportStatus status, gsize actual_length,
    gpointer user_data)
{
  FpcDisumSession *self = user_data;

  self->pending_ops = 0;

  if (self->state == ST_DONE)
    return;

  if (self->terminal_pending)
    {
      deliver_terminal (self);
      return;
    }

  if (self->in_close)
    {
      cleanup_out_done (self, status, actual_length);
      return;
    }

  if (reject_if_cancelled (self, status, 0))
    return;

  if (self->state == ST_CAPTURE_ARM
      && (status == FPC_DISUM_TRANSPORT_TIMEOUT
          || status == FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN))
    {
      /* The command was submitted and its delivery cannot be proven: cleanup is
       * conservative and sends ARM_STOP (SPEC §16.3). */
      self->arm_maybe_accepted = TRUE;
    }

  if (status == FPC_DISUM_TRANSPORT_TIMEOUT)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_IO, "The control transfer timed out");
      return;
    }

  if (reject_if_failed (self, status, 0))
    return;

  if (reject_if_connection_changed (self, 0))
    return;

  if (!accept_control_out (self, status, actual_length))
    return;

  switch (self->state)
    {
    case ST_INIT_INDICATE_S_STATE:
      self->state = ST_INIT_GET_STATE;
      break;
    case ST_INIT_CMD_INIT:
      {
        gint64 deadline_us = g_get_monotonic_time () + (gint64)self->reopen_ms * 1000;
        FpcDisumTransportStatus rebind = FPC_DISUM_TRANSPORT_OK;

        if (self->transport->init_rebind != NULL)
          rebind = self->transport->init_rebind (
              self->transport, FPC_DISUM_INIT_REBIND_ALLOW, deadline_us);
        if (rebind != FPC_DISUM_TRANSPORT_OK)
          {
            finish_operation (
                self, FPC_DISUM_SESSION_ERROR_IO, "The reclaim transaction was refused");
            return;
          }

        self->rebind_allowed = TRUE;
        self->reopen_deadline_us = deadline_us;
        /* The handle must be swapped before anything else is sent (SPEC §6). */
        self->state = ST_INIT_REOPEN;
      }
      break;
    case ST_INIT_TLS_INIT:
      /* The handshake deadline is absolute from this submit and is never
       * restarted by TLS_DATA progress (SPEC §16.2). */
      self->state = ST_HANDSHAKE;
      break;
    case ST_HANDSHAKE:
      /* A TLS_DATA fragment was accepted; the absolute window is untouched. */
      break;
    case ST_CAPTURE_ARM:
      self->armed = TRUE;
      arm_timer (self, self->window_ms);
      self->state = ST_CAPTURE_WAIT_TOUCH;

      /* The caller learns about the accepted ARM before anything else. */
      if (self->arm_complete_handler != NULL)
        self->arm_complete_handler (actual_length, self->arm_complete_data);

      if (self->state == ST_DONE || self->operation != OP_CAPTURE)
        return;
      break;
    case ST_CAPTURE_GET_IMG:
      arm_timer (self, self->window_ms);
      self->state = ST_CAPTURE_IMAGE;
      break;
    case ST_CLOSE_ARM_STOP:
    case ST_CLOSE_ABORT:
    case ST_CLOSE_SESSION_OFF:
      /* Handled by cleanup_out_done above. */
      return;
    /* No control transfer completes from these states. */
    case ST_NONE:
    case ST_INIT_GET_STATE:
    case ST_INIT_REOPEN:
    case ST_INIT_GET_TLS_KEY:
    case ST_CAPTURE_WAIT_TOUCH:
    case ST_CAPTURE_IMAGE:
    case ST_QUIESCE:
    case ST_DONE:
      return;
    }

  /* A TLS_DATA fragment completion simply continues the pump. */
  session_step (self);
}

static gboolean
accept_control_in (FpcDisumSession *self, FpcDisumTransportStatus status)
{
  return status == FPC_DISUM_TRANSPORT_OK;
}

static void
on_control_in (FpcDisumTransport *transport, FpcDisumTransportStatus status, const guint8 *data,
    gsize len, gpointer user_data)
{
  FpcDisumSession *self = user_data;

  self->pending_ops = 0;

  if (self->state == ST_DONE)
    return;

  if (self->terminal_pending)
    {
      deliver_terminal (self);
      return;
    }

  if (self->in_close)
    {
      /* No control IN belongs to the cleanup; drop a stale completion. */
      return;
    }

  if (reject_if_cancelled (self, status, len))
    return;

  /* GET_TLS_KEY may be retried once on a dead old connection, before TLS starts.
   * The command that was already answered is never re-sent after TLS starts. */
  if (status == FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE && self->state == ST_INIT_GET_TLS_KEY
      && !self->tls_started && !self->key_retry_used)
    {
      self->key_retry_used = TRUE;
      self->connection_dead = TRUE;
      note_generation (self);
      self->state = ST_INIT_REOPEN;
      session_step (self);
      return;
    }

  if (status == FPC_DISUM_TRANSPORT_TIMEOUT)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_IO, "The control transfer timed out");
      return;
    }

  if (reject_if_failed (self, status, len))
    return;

  if (reject_if_connection_changed (self, 0))
    return;

  if (!accept_control_in (self, status))
    return;

  switch (self->state)
    {
    case ST_INIT_GET_STATE:
      /* The body of this response is not part of the reviewed contract; it is
       * deliberately not interpreted (SPEC §10). */
      self->state = ST_INIT_CMD_INIT;
      break;
    case ST_INIT_GET_TLS_KEY:
      {
        FpcDisumEnvelopeStatus envelope_status;

        if (data == NULL || len != FPC_DISUM_ENVELOPE_LEN)
          {
            finish_operation (
                self, FPC_DISUM_SESSION_ERROR_ENVELOPE, "the key envelope has the wrong length");
            return;
          }

        envelope_status = fpc_disum_envelope_unseal (data, len, &self->psk);
        if (envelope_status != FPC_DISUM_ENVELOPE_OK)
          {
            finish_operation (
                self, FPC_DISUM_SESSION_ERROR_ENVELOPE, "the key envelope was rejected");
            return;
          }

        if (self->psk.psk_len != FPC_DISUM_PSK_LEN)
          {
            finish_operation (self, FPC_DISUM_SESSION_ERROR_ENVELOPE,
                "the sealed key is not the reviewed length");
            return;
          }

        self->tls = fpc_disum_tls_new (&self->psk);
        if (self->tls == NULL)
          {
            finish_operation (
                self, FPC_DISUM_SESSION_ERROR_TLS, "the security channel could not be created");
            return;
          }

        self->state = ST_INIT_TLS_INIT;
      }
      break;
    /* No control IN completes from these states. */
    case ST_NONE:
    case ST_INIT_INDICATE_S_STATE:
    case ST_INIT_CMD_INIT:
    case ST_INIT_REOPEN:
    case ST_INIT_TLS_INIT:
    case ST_HANDSHAKE:
    case ST_CAPTURE_ARM:
    case ST_CAPTURE_WAIT_TOUCH:
    case ST_CAPTURE_GET_IMG:
    case ST_CAPTURE_IMAGE:
    case ST_CLOSE_ARM_STOP:
    case ST_CLOSE_ABORT:
    case ST_CLOSE_SESSION_OFF:
    case ST_QUIESCE:
    case ST_DONE:
      return;
    }

  session_step (self);
}

static void
on_reopen (FpcDisumTransport *transport, FpcDisumTransportStatus status, gsize actual_length,
    gpointer user_data)
{
  FpcDisumSession *self = user_data;

  self->pending_ops = 0;

  if (self->state == ST_DONE)
    return;

  if (self->terminal_pending)
    {
      deliver_terminal (self);
      return;
    }

  if (self->in_close)
    return;

  if (reject_if_cancelled (self, status, 0))
    return;

  if (status == FPC_DISUM_TRANSPORT_TIMEOUT)
    {
      finish_operation (
          self, FPC_DISUM_SESSION_ERROR_IO, "The device did not come back after CMD_INIT");
      return;
    }

  if (reject_if_failed (self, status, 0))
    return;

  self->connection_dead = FALSE;
  note_generation (self);

  self->state = ST_INIT_GET_TLS_KEY;
  session_step (self);
}

/* ------------------------------------------------------------ TLS pumping */

static void
control_out (FpcDisumSession *self, guint8 request, guint16 value, const guint8 *data, gsize len)
{
  if (self->pending_ops > 0)
    {
      /* Exactly one operation is outstanding; never overwrite it. */
      return;
    }

  self->out_requested_length = len;
  self->cancel_requested = FALSE;
  self->pending_ops = 1;
  self->transport->control_out (self->transport, request, value, data, len, on_control_out, self);
}

static void control_in (FpcDisumSession *self, guint8 request, guint16 value, gsize requested,
    FpcDisumTransportDataFunc done);

static void on_read (FpcDisumTransport *transport, FpcDisumTransportStatus status,
    const guint8 *data, gsize len, gpointer user_data);

static void
read_endpoint (FpcDisumSession *self)
{
  if (self->pending_ops > 0)
    return;

  self->cancel_requested = FALSE;
  self->pending_ops = 1;
  self->transport->bulk_read (self->transport, on_read, self);
}

static void
submit_reopen (FpcDisumSession *self)
{
  gint64 now = g_get_monotonic_time ();

  if (self->pending_ops > 0)
    return;

  if (self->reopen_deadline_us != 0 && now >= self->reopen_deadline_us)
    {
      finish_operation (
          self, FPC_DISUM_SESSION_ERROR_IO, "The device did not come back after CMD_INIT");
      return;
    }

  if (self->reopen_deadline_us != 0)
    arm_timer_until (self, self->reopen_deadline_us);

  self->cancel_requested = FALSE;
  self->pending_ops = 1;
  self->transport->reopen (self->transport, self->reopen_deadline_us, on_reopen, self);
}

static void
control_in (FpcDisumSession *self, guint8 request, guint16 value, gsize requested,
    FpcDisumTransportDataFunc done)
{
  if (self->pending_ops > 0)
    return;

  self->cancel_requested = FALSE;
  self->pending_ops = 1;
  self->transport->control_in (self->transport, request, value, requested, done, self);
}

static gboolean
take_tls_output (FpcDisumSession *self)
{
  gsize written = fpc_disum_tls_take_output (self->tls, self->tls_chunk, sizeof self->tls_chunk);

  if (written == 0)
    return FALSE;

  /* Post-handshake output never restarts the capture window. */
  control_out (self, CMD_TLS_DATA, TLS_INIT_VALUE, self->tls_chunk, written);
  return TRUE;
}

static void
handshake_pump (FpcDisumSession *self)
{
  FpcDisumTlsStatus status;

  /* The server's final flight still has to be handed to the device. */
  if (fpc_disum_tls_is_established (self->tls))
    {
      if (take_tls_output (self))
        return;

      cancel_timer (self);
      finish_operation (self, FPC_DISUM_SESSION_OK, NULL);
      return;
    }

  status = fpc_disum_tls_step (self->tls);
  if (status == FPC_DISUM_TLS_ERROR_IDENTITY || status == FPC_DISUM_TLS_ERROR_HANDSHAKE
      || status == FPC_DISUM_TLS_ERROR_IO || status == FPC_DISUM_TLS_ERROR_CLOSED)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_TLS, fpc_disum_tls_error_text (self->tls));
      return;
    }

  if (take_tls_output (self))
    return;

  read_endpoint (self);
}

/* Feed decrypted application data into the message parser. */
static gboolean
drain_application_message (FpcDisumSession *self)
{
  gssize len;

  while ((len = fpc_disum_tls_read (
              self->tls, self->application_chunk, sizeof self->application_chunk))
         > 0)
    {
      if (!fpc_disum_parser_feed (&self->application, self->application_chunk, (gsize)len))
        {
          finish_operation (
              self, FPC_DISUM_SESSION_ERROR_PROTOCOL, fpc_disum_parser_error (&self->application));
          return TRUE;
        }

      if (self->state == ST_DONE)
        return TRUE;
    }

  if (len < 0)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_TLS, fpc_disum_tls_error_text (self->tls));
      return TRUE;
    }

  return FALSE;
}

/*
 * Drive everything the security channel can do without new device bytes: flush
 * queued output, process alerts/close_notify and (only after GET_IMG) accept
 * application data.  Returns TRUE when an operation was submitted or the session
 * ended, so the caller must not start another read.
 */
static gboolean
tls_process_pending (FpcDisumSession *self, gboolean application_allowed)
{
  gssize len;

  if (self->tls == NULL)
    return FALSE;

  if (!fpc_disum_tls_is_established (self->tls))
    {
      FpcDisumTlsStatus status = fpc_disum_tls_step (self->tls);

      if (status == FPC_DISUM_TLS_ERROR_IDENTITY || status == FPC_DISUM_TLS_ERROR_HANDSHAKE
          || status == FPC_DISUM_TLS_ERROR_IO || status == FPC_DISUM_TLS_ERROR_CLOSED)
        {
          finish_operation (
              self, FPC_DISUM_SESSION_ERROR_TLS, fpc_disum_tls_error_text (self->tls));
          return TRUE;
        }
    }

  if (take_tls_output (self))
    return TRUE;

  while ((len = fpc_disum_tls_read (
              self->tls, self->application_chunk, sizeof self->application_chunk))
         > 0)
    {
      if (!application_allowed)
        {
          /* The device must not send application data before GET_IMG (SPEC §4.5). */
          finish_operation (
              self, FPC_DISUM_SESSION_ERROR_PROTOCOL, "application data arrived before GET_IMG");
          return TRUE;
        }

      if (!fpc_disum_parser_feed (&self->application, self->application_chunk, (gsize)len))
        {
          finish_operation (
              self, FPC_DISUM_SESSION_ERROR_PROTOCOL, fpc_disum_parser_error (&self->application));
          return TRUE;
        }

      if (self->state == ST_DONE)
        return TRUE;

      if (take_tls_output (self))
        return TRUE;
    }

  if (len < 0)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_TLS, fpc_disum_tls_error_text (self->tls));
      return TRUE;
    }

  return self->pending_ops > 0;
}

/* --------------------------------------------------------- frame handling */

static void
emit_event_record (FpcDisumSession *self, const FpcDisumFrame *frame)
{
  FpcDisumDiagnosticRecord record = { 0 };

  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_EVENT;
  record.event_valid = TRUE;
  record.event_code = frame->code;
  record.total_len = frame->total_len;
  record.third_field = frame->third_field;
  emit_record (self, &record);
}

static void
on_application_frame (const FpcDisumFrame *frame, gpointer user_data)
{
  FpcDisumSession *self = user_data;
  FpcDisumFrame copy;
  FpcDisumImage image;

  if (self->state == ST_DONE)
    return;

  /* Only a capture that followed an accepted GET_IMG may deliver an image. */
  if (self->state != ST_CAPTURE_IMAGE || self->cancelled || !self->armed)
    {
      finish_operation (
          self, FPC_DISUM_SESSION_ERROR_PROTOCOL, "application data arrived before GET_IMG");
      return;
    }

  /* The parser's view is only valid inside this callback. */
  g_byte_array_set_size (self->application_message, 0);
  g_byte_array_append (self->application_message, frame->payload, frame->payload_len);

  copy.code = frame->code;
  copy.total_len = frame->total_len;
  copy.third_field = frame->third_field;
  copy.payload_len = frame->payload_len;
  copy.payload = self->application_message->data;

  if (!fpc_disum_frame_get_image (&copy, &image))
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_PROTOCOL,
          "the application message is not the reviewed image");
      return;
    }

  self->image = image.pixels;
  self->image_len = image.pixel_len;

  finish_operation (self, FPC_DISUM_SESSION_OK, NULL);
}

static void
on_event_frame (const FpcDisumFrame *frame, gpointer user_data)
{
  FpcDisumSession *self = user_data;
  gboolean used = FALSE;

  if (self->state == ST_DONE)
    return;

  emit_event_record (self, frame);

  switch (frame->code)
    {
    case EVT_TLS:
      /* The security channel is driven in WAIT_TOUCH too (SPEC §8). */
      if (self->state == ST_HANDSHAKE || self->state == ST_CAPTURE_WAIT_TOUCH
          || self->state == ST_CAPTURE_IMAGE)
        {
          if (self->tls == NULL
              || !fpc_disum_tls_feed (self->tls, frame->payload, frame->payload_len))
            {
              record_event (self, frame);
              finish_operation (self, FPC_DISUM_SESSION_ERROR_TLS,
                  self->tls != NULL ? fpc_disum_tls_error_text (self->tls) : NULL);
              return;
            }

          used = TRUE;
        }
      break;
    case EVT_FINGER_DWN:
      if (self->state == ST_CAPTURE_WAIT_TOUCH)
        {
          self->state = ST_CAPTURE_GET_IMG;
          used = TRUE;

          /* The caller learns about the touch before GET_IMG goes out. */
          if (self->touch_handler != NULL)
            self->touch_handler (self->touch_data);

          /* A handler that cancels or closes must not be followed by GET_IMG. */
          if (self->state == ST_DONE || self->cancelled || self->terminal_pending)
            return;
        }
      break;
    case EVT_ARM_RESULT:
      /* The reserved state value fails the transaction; it is never named
       * (SPEC §16.1). */
      if (frame->third_field == ARM_RESULT_RESERVED)
        {
          record_event (self, frame);
          finish_operation (self, FPC_DISUM_SESSION_ERROR_ARM_RESULT,
              "the arm result carries the reserved state value");
          return;
        }
      break;
    case EVT_HELLO:
    case EVT_INIT_RESULT:
    case EVT_DEAD_PIXEL_REPORT:
    case EVT_FINGER_UP:
    case EVT_USB_LOGS:
    case EVT_TLS_KEY:
    case EVT_REFRESH_SENSOR:
      /* Reviewed and irrelevant here: record and ignore (SPEC §16.1). */
      break;
    default:
      record_event (self, frame);
      finish_operation (self, FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT,
          "an event outside the reviewed table arrived");
      return;
    }

  if (!used)
    record_event (self, frame);
}

static void
on_read (FpcDisumTransport *transport, FpcDisumTransportStatus status, const guint8 *data,
    gsize len, gpointer user_data)
{
  FpcDisumSession *self = user_data;

  self->pending_ops = 0;

  if (self->state == ST_DONE)
    return;

  /* A terminal outcome is already recorded: the tail is discarded, not parsed. */
  if (self->terminal_pending)
    {
      self->discarded_length += len;
      deliver_terminal (self);
      return;
    }

  if (reject_if_cancelled (self, status, len))
    return;

  if (status == FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE || status == FPC_DISUM_TRANSPORT_ERROR_IO
      || status == FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN)
    {
      reject_if_failed (self, status, len);
      return;
    }

  if (reject_if_connection_changed (self, len))
    return;

  /* A TIMEOUT may still carry the bytes it received; feed them in order and keep
   * the original deadline.  TIMEOUT+0 and a ZLP never clear a partial frame. */
  if (len > 0)
    {
      if (!fpc_disum_parser_feed (&self->events, data, len))
        {
          finish_operation (
              self, FPC_DISUM_SESSION_ERROR_PROTOCOL, fpc_disum_parser_error (&self->events));
          return;
        }
    }

  /* The frame callbacks may have finished the operation or moved the state on. */
  if (self->state == ST_DONE || self->terminal_pending)
    return;

  session_step (self);
}

/* ------------------------------------------------------------ state runner */

static void
run_state (FpcDisumSession *self)
{
  if (self->pending_ops > 0 || self->terminal_pending || self->state == ST_DONE)
    return;

  if (self->cancelled && !self->in_close)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_CANCELLED, "cancelled");
      return;
    }

  update_phase (self);

  switch (self->state)
    {
    case ST_INIT_INDICATE_S_STATE:
      control_out (self, CMD_INDICATE_S_STATE, INDICATE_S_STATE_VALUE, NULL, 0);
      break;
    case ST_INIT_GET_STATE:
      control_in (self, CMD_GET_STATE, 0, GET_STATE_CAPACITY, on_control_in);
      break;
    case ST_INIT_CMD_INIT:
      {
        /* The reviewed default payload is four zero bytes (SPEC §3.1). */
        static const guint8 payload[INIT_PAYLOAD_LEN] = { 0 };
        FpcDisumTransportStatus rebind = FPC_DISUM_TRANSPORT_OK;

        /* CMD_INIT makes the device leave the bus and come back (SPEC §6 step 4),
         * so the initialisation transaction starts before the command goes out. */
        if (!self->rebind_open)
          {
            FpcDisumTransportIdentity identity = current_identity (self);

            if (self->transport->init_rebind != NULL)
              rebind
                  = self->transport->init_rebind (self->transport, FPC_DISUM_INIT_REBIND_BEGIN, 0);
            if (rebind != FPC_DISUM_TRANSPORT_OK)
              {
                finish_operation (
                    self, FPC_DISUM_SESSION_ERROR_IO, "The reclaim transaction could not begin");
                return;
              }

            self->rebind_open = TRUE;
            self->generation = identity.generation;
          }

        control_out (self, CMD_INIT, INIT_VALUE, payload, sizeof payload);
      }
      break;
    case ST_INIT_REOPEN:
      submit_reopen (self);
      break;
    case ST_INIT_GET_TLS_KEY:
      control_in (self, CMD_GET_TLS_KEY, 0, GET_TLS_KEY_CAPACITY, on_control_in);
      break;
    case ST_INIT_TLS_INIT:
      {
        FpcDisumTransportStatus rebind = FPC_DISUM_TRANSPORT_OK;
        FpcDisumTransportIdentity identity;

        /* The reclaimed transport is committed before TLS_INIT is submitted. */
        if (!self->rebind_committed)
          {
            if (self->transport->init_rebind != NULL)
              rebind = self->transport->init_rebind (
                  self->transport, FPC_DISUM_INIT_REBIND_COMMIT, self->reopen_deadline_us);
            if (rebind != FPC_DISUM_TRANSPORT_OK)
              {
                finish_operation (self, FPC_DISUM_SESSION_ERROR_IO,
                    "The reclaimed device could not be committed");
                return;
              }

            self->rebind_committed = TRUE;
            self->rebind_open = FALSE;
            self->rebind_allowed = FALSE;
          }

        identity = current_identity (self);
        self->tls_started = TRUE;
        self->tls_generation = identity.generation;
        arm_timer (self, self->handshake_ms);
        control_out (self, CMD_TLS_INIT, TLS_INIT_VALUE, NULL, 0);
      }
      break;
    case ST_HANDSHAKE:
      handshake_pump (self);
      break;
    case ST_CAPTURE_ARM:
      self->arm_submitted = TRUE;
      control_out (self, CMD_ARM, ARM_MODE_VERIFY, arm_payload, sizeof arm_payload);
      break;
    case ST_CAPTURE_WAIT_TOUCH:
      if (tls_process_pending (self, FALSE))
        break;
      read_endpoint (self);
      break;
    case ST_CAPTURE_GET_IMG:
      control_out (self, CMD_GET_IMG, 0, NULL, 0);
      break;
    case ST_CAPTURE_IMAGE:
      if (tls_process_pending (self, TRUE))
        break;

      read_endpoint (self);
      break;
    case ST_CLOSE_ARM_STOP:
    case ST_CLOSE_ABORT:
    case ST_CLOSE_SESSION_OFF:
      /* Submitted by cleanup_advance(). */
      break;
    case ST_NONE:
    case ST_QUIESCE:
    case ST_DONE:
      break;
    }
}

/*
 * One step of the state machine.  Transfers may complete synchronously, which
 * would otherwise recurse once per operation; a nested step is folded into this
 * loop instead.
 */
static void
session_step (FpcDisumSession *self)
{
  if (self->stepping)
    {
      self->step_again = TRUE;
      return;
    }

  self->stepping = TRUE;
  do
    {
      self->step_again = FALSE;
      run_state (self);
    }
  while (self->step_again);
  self->stepping = FALSE;
}

static gboolean
on_timeout (gpointer user_data)
{
  FpcDisumSession *self = user_data;

  self->timeout_handle = NULL;

  switch (self->state)
    {
    case ST_INIT_REOPEN:
      finish_operation (
          self, FPC_DISUM_SESSION_ERROR_IO, "The device did not come back after CMD_INIT");
      break;
    case ST_HANDSHAKE:
      finish_operation (self, FPC_DISUM_SESSION_ERROR_TLS, "the handshake made no progress");
      break;
    case ST_CAPTURE_WAIT_TOUCH:
      finish_operation (
          self, FPC_DISUM_SESSION_ERROR_NO_TOUCH, "no touch event arrived within the window");
      break;
    case ST_CAPTURE_IMAGE:
      finish_operation (
          self, FPC_DISUM_SESSION_ERROR_NO_IMAGE, "no image arrived within the window");
      break;
    /* A window is only armed while one of the states above is waiting. */
    case ST_NONE:
    case ST_INIT_INDICATE_S_STATE:
    case ST_INIT_GET_STATE:
    case ST_INIT_CMD_INIT:
    case ST_INIT_GET_TLS_KEY:
    case ST_INIT_TLS_INIT:
    case ST_CAPTURE_ARM:
    case ST_CAPTURE_GET_IMG:
    case ST_CLOSE_ARM_STOP:
    case ST_CLOSE_ABORT:
    case ST_CLOSE_SESSION_OFF:
    case ST_QUIESCE:
    case ST_DONE:
      break;
    }

  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------- cleanup */

static void
cleanup_submit (FpcDisumSession *self)
{
  CleanupRecord *cleanup;
  const guint8 *payload = NULL;
  guint16 value = 0;

  if (self->cleanup_index >= CLEANUP_COUNT)
    {
      finish_close (self, FPC_DISUM_SESSION_OK, NULL);
      return;
    }

  cleanup = &self->cleanup[self->cleanup_index];

  switch (self->cleanup_index)
    {
    case CLEANUP_ARM_STOP:
      self->state = ST_CLOSE_ARM_STOP;
      self->arm_stop_sent = TRUE;
      payload = arm_stop_payload;
      value = ARM_MODE_VERIFY;
      cleanup->requested_length = sizeof arm_stop_payload;
      control_out (self, CMD_ARM, value, payload, sizeof arm_stop_payload);
      break;
    case CLEANUP_ABORT:
      self->state = ST_CLOSE_ABORT;
      cleanup->requested_length = 0;
      control_out (self, CMD_ABORT, 0, NULL, 0);
      break;
    case CLEANUP_SESSION_OFF:
      self->state = ST_CLOSE_SESSION_OFF;
      cleanup->requested_length = 0;
      control_out (self, CMD_SESSION_OFF, 0, NULL, 0);
      break;
    default:
      g_assert_not_reached ();
      break;
    }

  update_phase (self);
}

static void
cleanup_advance (FpcDisumSession *self)
{
  self->cleanup_index++;

  if (self->cleanup_index == CLEANUP_COUNT)
    {
      finish_close (self, self->cleanup_failed ? FPC_DISUM_SESSION_ERROR_IO : FPC_DISUM_SESSION_OK,
          self->cleanup_failed ? "a cleanup command failed" : NULL);
      return;
    }

  cleanup_submit (self);
}

static void
begin_cleanup (FpcDisumSession *self)
{
  for (guint i = 0; i < CLEANUP_COUNT; i++)
    {
      self->cleanup[i].attempted = FALSE;
      self->cleanup[i].submitted = FALSE;
      self->cleanup[i].status = FPC_DISUM_TRANSPORT_OK;
      self->cleanup[i].requested_length
          = i == CLEANUP_ARM_STOP ? (gsize)sizeof arm_stop_payload : 0;
      self->cleanup[i].actual_known = FALSE;
      self->cleanup[i].actual_length = 0;
      self->cleanup[i].result = NULL;
    }

  self->cleanup_index = CLEANUP_ARM_STOP;
  self->cleanup_failed = FALSE;
  self->in_close = TRUE;
  self->state = ST_CLOSE_ABORT;

  update_phase (self);

  if (!self->armed && !self->arm_maybe_accepted)
    {
      /* No ARM_STOP is needed; nothing was armed and no command may have
       * reached the device.  The record says so instead of inventing success. */
      self->cleanup[CLEANUP_ARM_STOP].attempted = FALSE;
      self->cleanup[CLEANUP_ARM_STOP].submitted = FALSE;
      self->cleanup[CLEANUP_ARM_STOP].result = "not-needed";
      cleanup_emit (self, CLEANUP_ARM_STOP);
      self->cleanup_index = CLEANUP_ABORT;
    }

  if (self->connection_dead)
    {
      for (guint i = 0; i < CLEANUP_COUNT; i++)
        {
          self->cleanup[i].attempted = FALSE;
          self->cleanup[i].submitted = FALSE;
          self->cleanup[i].status = FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE;
          self->cleanup[i].actual_known = FALSE;
          self->cleanup[i].result = "unavailable";
          cleanup_emit (self, i);
        }

      finish_close (self, FPC_DISUM_SESSION_ERROR_IO, "The device left the bus");
      return;
    }

  if (self->cleanup_index >= CLEANUP_COUNT)
    {
      finish_close (self, FPC_DISUM_SESSION_OK, NULL);
      return;
    }

  cleanup_submit (self);
}

/* ------------------------------------------------------------- public API */

FpcDisumSession *
fpc_disum_session_new (FpcDisumTransport *transport)
{
  FpcDisumSession *self;

  g_return_val_if_fail (transport != NULL, NULL);
  g_return_val_if_fail (transport->control_out != NULL, NULL);
  g_return_val_if_fail (transport->control_in != NULL, NULL);
  g_return_val_if_fail (transport->bulk_read != NULL, NULL);
  g_return_val_if_fail (transport->reopen != NULL, NULL);
  g_return_val_if_fail (transport->cancel != NULL, NULL);

  self = g_new0 (FpcDisumSession, 1);
  self->transport = transport;
  self->timeouts = glib_timeouts;
  self->handshake_ms = FPC_DISUM_HANDSHAKE_TIMEOUT_MS;
  self->reopen_ms = FPC_DISUM_REOPEN_TIMEOUT_MS;
  self->window_ms = FPC_DISUM_CAPTURE_WINDOW_MS;
  self->application_message = g_byte_array_new ();

  fpc_disum_parser_init (&self->events, FPC_DISUM_STREAM_EVENT, on_event_frame, self);
  fpc_disum_parser_init (
      &self->application, FPC_DISUM_STREAM_APPLICATION, on_application_frame, self);

  return self;
}

void
fpc_disum_session_free (FpcDisumSession *self)
{
  /* A session still running would leave a callback pointing at freed memory. */
  g_return_if_fail (self->state == ST_NONE || self->state == ST_DONE);
  g_return_if_fail (self->pending_ops == 0);

  cancel_timer (self);
  release_channel (self);
  abort_rebind (self);
  fpc_disum_parser_clear (&self->events);
  fpc_disum_parser_clear (&self->application);
  g_clear_pointer (&self->application_message, g_byte_array_unref);
  g_clear_pointer (&self->diagnostics, fpc_disum_diagnostics_unref);
  g_free (self);
}

void
fpc_disum_session_set_timeout_source (FpcDisumSession *self, const FpcDisumTimeoutSource *source)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->state == ST_NONE);

  if (source == NULL)
    {
      self->timeouts = glib_timeouts;
      return;
    }

  g_return_if_fail (source->add != NULL);
  g_return_if_fail (source->remove != NULL);

  self->timeouts = *source;
}

void
fpc_disum_session_set_timeouts (
    FpcDisumSession *self, guint handshake_ms, guint reopen_ms, guint capture_window_ms)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->state == ST_NONE);

  if (handshake_ms > 0)
    self->handshake_ms = handshake_ms;
  if (reopen_ms > 0)
    self->reopen_ms = reopen_ms;
  if (capture_window_ms > 0)
    self->window_ms = capture_window_ms;
}

void
fpc_disum_session_set_touch_handler (
    FpcDisumSession *self, FpcDisumTouchFunc handler, gpointer user_data)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->state == ST_NONE);

  self->touch_handler = handler;
  self->touch_data = user_data;
}

void
fpc_disum_session_set_arm_complete_handler (
    FpcDisumSession *self, FpcDisumArmCompleteFunc handler, gpointer user_data)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->state == ST_NONE);

  self->arm_complete_handler = handler;
  self->arm_complete_data = user_data;
}

void
fpc_disum_session_set_diagnostics (FpcDisumSession *self, FpcDisumDiagnostics *diagnostics)
{
  g_return_if_fail (self != NULL);

  if (diagnostics != NULL)
    fpc_disum_diagnostics_ref (diagnostics);

  g_clear_pointer (&self->diagnostics, fpc_disum_diagnostics_unref);
  self->diagnostics = diagnostics;

  if (self->diagnostics != NULL)
    fpc_disum_diagnostics_set_phase (self->diagnostics, self->phase);
}

/* A session already ended by an abnormal reset refuses automatic recovery with
 * no USB traffic; only a new session may try again (SPEC §16.2). */
static gboolean
reject_poisoned (FpcDisumSession *self)
{
  /* The abnormal reset stays the session's first cause; recovery is simply not
   * available on this session and no USB request is made. */
  return self->abnormal_reset_recorded;
}

void
fpc_disum_session_init_async (
    FpcDisumSession *self, FpcDisumSessionDoneFunc done, gpointer user_data)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (done != NULL);

  if (reject_poisoned (self))
    {
      done (FPC_DISUM_SESSION_ERROR_UNSUPPORTED_RESET_RECOVERY, NULL, user_data);
      return;
    }

  g_return_if_fail (self->state == ST_NONE);
  g_return_if_fail (self->operation == OP_NONE);

  self->operation = OP_INIT;
  self->done = done;
  self->done_data = user_data;
  self->state = ST_INIT_INDICATE_S_STATE;

  session_step (self);
}

void
fpc_disum_session_capture_async (
    FpcDisumSession *self, FpcDisumSessionCaptureFunc done, gpointer user_data)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (done != NULL);

  if (reject_poisoned (self))
    {
      FpcDisumCaptureResult result = {
        .status = FPC_DISUM_SESSION_ERROR_UNSUPPORTED_RESET_RECOVERY,
        .message = NULL,
        .image = NULL,
        .image_len = 0,
      };

      done (&result, user_data);
      return;
    }

  g_return_if_fail (self->operation == OP_NONE);
  /* A capture needs the channel the initialisation established. */
  g_return_if_fail (self->tls != NULL);
  g_return_if_fail (fpc_disum_tls_is_established (self->tls));

  self->operation = OP_CAPTURE;
  self->capture_done = done;
  self->done_data = user_data;
  self->state = ST_CAPTURE_ARM;

  session_step (self);
}

void
fpc_disum_session_close_async (
    FpcDisumSession *self, FpcDisumSessionDoneFunc done, gpointer user_data)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->operation == OP_NONE);
  g_return_if_fail (self->pending_ops == 0);
  g_return_if_fail (!self->terminal_pending);
  g_return_if_fail (self->state == ST_DONE || self->state == ST_NONE);

  self->operation = OP_CLOSE;
  self->done = done;
  self->done_data = user_data;

  begin_cleanup (self);
}

void
fpc_disum_session_cancel (FpcDisumSession *self)
{
  g_return_if_fail (self != NULL);

  if (self->cancelled)
    return;

  self->cancelled = TRUE;

  /* The window must not turn a cancellation into a timeout. */
  cancel_timer (self);

  if (self->state == ST_DONE || self->state == ST_NONE || self->in_close)
    return;

  if (self->terminal_pending)
    return; /* the first cause is already fixed */

  if (self->pending_ops > 0)
    {
      /* Wait for the real completion; the cancel call is not a completed I/O. */
      if (!self->cancel_requested)
        {
          self->cancel_requested = TRUE;
          self->transport->cancel (self->transport);
        }

      return;
    }

  finish_operation (self, FPC_DISUM_SESSION_ERROR_CANCELLED, "cancelled");
}

gboolean
fpc_disum_session_is_cancelled (const FpcDisumSession *self)
{
  return self->cancelled;
}

gsize
fpc_disum_session_event_count (const FpcDisumSession *self)
{
  return self->n_events;
}

const FpcDisumSessionEvent *
fpc_disum_session_event (const FpcDisumSession *self, gsize index)
{
  if (index >= self->n_events)
    return NULL;

  return &self->event_log[index];
}

gboolean
fpc_disum_session_was_armed (const FpcDisumSession *self)
{
  return self->armed || self->arm_maybe_accepted || self->arm_stop_sent;
}
