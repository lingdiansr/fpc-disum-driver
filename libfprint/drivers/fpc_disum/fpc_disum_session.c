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
  ST_DONE,
} SessionState;

typedef enum
{
  OP_NONE = 0,
  OP_INIT,
  OP_CAPTURE,
  OP_CLOSE,
} OperationKind;

struct _FpcDisumSession
{
  FpcDisumTransport *transport;
  FpcDisumTimeoutSource timeouts;

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

  /* First root cause; later failures are recorded but never replace it. */
  FpcDisumSessionStatus failure;
  const gchar *failure_message;

  FpcDisumSessionDoneFunc done;
  gpointer done_data;
  FpcDisumSessionCaptureFunc capture_done;

  FpcDisumTouchFunc touch_handler;
  gpointer touch_data;

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

  guint8 tls_chunk[FPC_DISUM_TLS_DATA_MAX];
  guint8 application_chunk[APPLICATION_CHUNK];
};

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

/* --------------------------------------------------------------- stepping */

static void session_step (FpcDisumSession *self);

static const gchar *
drop_reason (FpcDisumTransportStatus status)
{
  return status == FPC_DISUM_TRANSPORT_ERROR_CANCELLED ? "cancelled" : "USB transfer failed";
}

static void
finish_operation (FpcDisumSession *self, FpcDisumSessionStatus status, const gchar *message)
{
  OperationKind operation = self->operation;
  FpcDisumSessionDoneFunc done = self->done;
  FpcDisumSessionCaptureFunc capture_done = self->capture_done;
  gpointer user_data = self->done_data;

  self->state = ST_DONE;
  self->operation = OP_NONE;
  self->done = NULL;
  self->capture_done = NULL;
  self->done_data = NULL;

  cancel_timer (self);
  record_failure (self, status, message);

  /* Abandon a read that is still in flight; its completion is ignored. */
  self->transport->cancel (self->transport);

  if (operation == OP_CAPTURE && capture_done != NULL)
    {
      FpcDisumCaptureResult result = {
        .status = status,
        .message = message,
        .image = status == FPC_DISUM_SESSION_OK ? self->image : NULL,
        .image_len = status == FPC_DISUM_SESSION_OK ? self->image_len : 0,
      };

      capture_done (&result, user_data);
      return;
    }

  if (done != NULL)
    done (status, message, user_data);
}

static void
finish_close (FpcDisumSession *self, FpcDisumSessionStatus status, const gchar *message)
{
  FpcDisumSessionDoneFunc done = self->done;
  gpointer user_data = self->done_data;

  self->state = ST_DONE;
  self->operation = OP_NONE;
  self->done = NULL;
  self->done_data = NULL;

  cancel_timer (self);
  record_failure (self, status, message);
  release_channel (self);

  if (done != NULL)
    done (self->failure, self->failure_message, user_data);
}

/*
 * Handle the outcome of a transfer.  Returns TRUE when the completion was
 * consumed (the operation finished or a stale callback was dropped), FALSE when
 * the state machine should carry on.
 */
static gboolean
handle_status (FpcDisumSession *self, FpcDisumTransportStatus status)
{
  if (self->state == ST_DONE)
    return TRUE;

  if (self->in_close)
    {
      /* Every cleanup command is attempted; a failure is recorded but never
       * replaces the first root cause (SPEC §16.3). */
      if (status != FPC_DISUM_TRANSPORT_OK)
        record_failure (self, FPC_DISUM_SESSION_ERROR_IO, "cleanup command failed");

      return FALSE;
    }

  if (self->cancelled || status == FPC_DISUM_TRANSPORT_ERROR_CANCELLED)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_CANCELLED, drop_reason (status));
      return TRUE;
    }

  if (status != FPC_DISUM_TRANSPORT_OK)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_IO, drop_reason (status));
      return TRUE;
    }

  return FALSE;
}

/* ------------------------------------------------------------- callbacks */

static void
on_control_out (FpcDisumTransport *transport, FpcDisumTransportStatus status, gpointer user_data)
{
  FpcDisumSession *self = user_data;

  if (handle_status (self, status))
    return;

  switch (self->state)
    {
    case ST_INIT_INDICATE_S_STATE:
      self->state = ST_INIT_GET_STATE;
      break;
    case ST_INIT_CMD_INIT:
      /* The handle must be swapped before anything else is sent (SPEC §6). */
      self->state = ST_INIT_REOPEN;
      break;
    case ST_INIT_TLS_INIT:
      arm_timer (self, self->handshake_ms);
      self->state = ST_HANDSHAKE;
      break;
    case ST_HANDSHAKE:
      /* A TLS_DATA fragment was accepted: progress, so the window restarts. */
      arm_timer (self, self->handshake_ms);
      break;
    case ST_CAPTURE_ARM:
      self->armed = TRUE;
      arm_timer (self, self->window_ms);
      self->state = ST_CAPTURE_WAIT_TOUCH;
      break;
    case ST_CAPTURE_GET_IMG:
      arm_timer (self, self->window_ms);
      self->state = ST_CAPTURE_IMAGE;
      break;
    case ST_CLOSE_ARM_STOP:
      self->state = ST_CLOSE_ABORT;
      break;
    case ST_CLOSE_ABORT:
      self->state = ST_CLOSE_SESSION_OFF;
      break;
    case ST_CLOSE_SESSION_OFF:
      finish_close (self, FPC_DISUM_SESSION_OK, NULL);
      return;
    /* No control transfer completes from these states. */
    case ST_NONE:
    case ST_INIT_GET_STATE:
    case ST_INIT_REOPEN:
    case ST_INIT_GET_TLS_KEY:
    case ST_CAPTURE_WAIT_TOUCH:
    case ST_CAPTURE_IMAGE:
    case ST_DONE:
      return;
    }

  session_step (self);
}

static void
on_control_in (FpcDisumTransport *transport, FpcDisumTransportStatus status, const guint8 *data,
    gsize len, gpointer user_data)
{
  FpcDisumSession *self = user_data;

  if (handle_status (self, status))
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
    case ST_DONE:
      return;
    }

  session_step (self);
}

static void
on_reopen (FpcDisumTransport *transport, FpcDisumTransportStatus status, gpointer user_data)
{
  FpcDisumSession *self = user_data;

  if (handle_status (self, status))
    return;

  self->state = ST_INIT_GET_TLS_KEY;
  session_step (self);
}

/* ------------------------------------------------------------ TLS pumping */

static void
control_out (FpcDisumSession *self, guint8 request, guint16 value, const guint8 *data, gsize len)
{
  self->transport->control_out (self->transport, request, value, data, len, on_control_out, self);
}

static void on_read (FpcDisumTransport *transport, FpcDisumTransportStatus status,
    const guint8 *data, gsize len, gpointer user_data);

static void
read_endpoint (FpcDisumSession *self)
{
  self->transport->bulk_read (self->transport, on_read, self);
}

static void
handshake_pump (FpcDisumSession *self)
{
  FpcDisumTlsStatus status;
  gsize written;

  /* The server's final flight still has to be handed to the device. */
  if (fpc_disum_tls_is_established (self->tls))
    {
      written = fpc_disum_tls_take_output (self->tls, self->tls_chunk, sizeof self->tls_chunk);
      if (written > 0)
        {
          control_out (self, CMD_TLS_DATA, TLS_INIT_VALUE, self->tls_chunk, written);
          return;
        }

      cancel_timer (self);
      finish_operation (self, FPC_DISUM_SESSION_OK, NULL);
      return;
    }

  status = fpc_disum_tls_step (self->tls);
  if (status == FPC_DISUM_TLS_ERROR_IDENTITY || status == FPC_DISUM_TLS_ERROR_HANDSHAKE
      || status == FPC_DISUM_TLS_ERROR_IO)
    {
      finish_operation (self, FPC_DISUM_SESSION_ERROR_TLS, fpc_disum_tls_error_text (self->tls));
      return;
    }

  written = fpc_disum_tls_take_output (self->tls, self->tls_chunk, sizeof self->tls_chunk);
  if (written > 0)
    {
      control_out (self, CMD_TLS_DATA, TLS_INIT_VALUE, self->tls_chunk, written);
      return;
    }

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

/* --------------------------------------------------------- frame handling */

static void
on_application_frame (const FpcDisumFrame *frame, gpointer user_data)
{
  FpcDisumSession *self = user_data;
  FpcDisumFrame copy;
  FpcDisumImage image;

  if (self->state == ST_DONE)
    return;

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

  switch (frame->code)
    {
    case EVT_TLS:
      if (self->state == ST_HANDSHAKE || self->state == ST_CAPTURE_IMAGE)
        {
          if (!fpc_disum_tls_feed (self->tls, frame->payload, frame->payload_len))
            {
              record_event (self, frame);
              finish_operation (
                  self, FPC_DISUM_SESSION_ERROR_TLS, fpc_disum_tls_error_text (self->tls));
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

  if (handle_status (self, status))
    return;

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
  if (self->state == ST_DONE)
    return;

  session_step (self);
}

/* ------------------------------------------------------------ state runner */

static void
run_state (FpcDisumSession *self)
{
  switch (self->state)
    {
    case ST_INIT_INDICATE_S_STATE:
      control_out (self, CMD_INDICATE_S_STATE, INDICATE_S_STATE_VALUE, NULL, 0);
      break;
    case ST_INIT_GET_STATE:
      self->transport->control_in (
          self->transport, CMD_GET_STATE, 0, GET_STATE_CAPACITY, on_control_in, self);
      break;
    case ST_INIT_CMD_INIT:
      {
        /* The reviewed default payload is four zero bytes (SPEC §3.1). */
        static const guint8 payload[INIT_PAYLOAD_LEN] = { 0 };

        /* CMD_INIT makes the device leave the bus and come back (SPEC §6 step 4),
         * so the transport is told to expect that before the command goes out;
         * it releases the expectation when the reconnect finishes. */
        self->transport->reenumeration_expected (self->transport, TRUE);
        control_out (self, CMD_INIT, INIT_VALUE, payload, sizeof payload);
      }
      break;
    case ST_INIT_REOPEN:
      arm_timer (self, self->reopen_ms);
      self->transport->reopen (self->transport, on_reopen, self);
      break;
    case ST_INIT_GET_TLS_KEY:
      self->transport->control_in (
          self->transport, CMD_GET_TLS_KEY, 0, GET_TLS_KEY_CAPACITY, on_control_in, self);
      break;
    case ST_INIT_TLS_INIT:
      control_out (self, CMD_TLS_INIT, TLS_INIT_VALUE, NULL, 0);
      break;
    case ST_HANDSHAKE:
      handshake_pump (self);
      break;
    case ST_CAPTURE_ARM:
      control_out (self, CMD_ARM, ARM_MODE_VERIFY, arm_payload, sizeof arm_payload);
      break;
    case ST_CAPTURE_WAIT_TOUCH:
      read_endpoint (self);
      break;
    case ST_CAPTURE_GET_IMG:
      control_out (self, CMD_GET_IMG, 0, NULL, 0);
      break;
    case ST_CAPTURE_IMAGE:
      /* Application data that is already buffered is decoded first. */
      if (drain_application_message (self))
        break;

      read_endpoint (self);
      break;
    case ST_CLOSE_ARM_STOP:
      control_out (self, CMD_ARM, ARM_MODE_VERIFY, arm_stop_payload, sizeof arm_stop_payload);
      break;
    case ST_CLOSE_ABORT:
      control_out (self, CMD_ABORT, 0, NULL, 0);
      break;
    case ST_CLOSE_SESSION_OFF:
      control_out (self, CMD_SESSION_OFF, 0, NULL, 0);
      break;
    case ST_NONE:
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
          self, FPC_DISUM_SESSION_ERROR_IO, "the device did not come back after CMD_INIT");
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
    case ST_DONE:
      break;
    }

  return G_SOURCE_REMOVE;
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
  g_return_val_if_fail (transport->reenumeration_expected != NULL, NULL);
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

  cancel_timer (self);
  release_channel (self);
  fpc_disum_parser_clear (&self->events);
  fpc_disum_parser_clear (&self->application);
  g_clear_pointer (&self->application_message, g_byte_array_unref);
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
fpc_disum_session_init_async (
    FpcDisumSession *self, FpcDisumSessionDoneFunc done, gpointer user_data)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (done != NULL);
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
  g_return_if_fail (self->state == ST_DONE || self->state == ST_NONE);

  self->operation = OP_CLOSE;
  self->in_close = TRUE;
  self->done = done;
  self->done_data = user_data;
  self->state = self->armed ? ST_CLOSE_ARM_STOP : ST_CLOSE_ABORT;

  session_step (self);
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

  /* An operation that is in flight completes with CANCELLED; a session that is
   * already finished has nothing to abandon. */
  if (self->state != ST_DONE && self->state != ST_NONE && !self->in_close)
    self->transport->cancel (self->transport);
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
  return self->armed;
}
