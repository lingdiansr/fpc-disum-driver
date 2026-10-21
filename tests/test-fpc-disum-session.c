/*
 * FPC Disum single-capture session tests driven by a scripted in-process device.
 *
 * The device model answers the reviewed control protocol, runs the device side of
 * the TLS-PSK handshake with the synthetic PSK from the review corpus, streams
 * EVT_TLS frames and models the transport completion contract: every operation is
 * submitted and completed exactly once, with a real status and actual length.  A
 * cancel request never doubles as a completed I/O: the default mode completes the
 * held operation, the delayed mode waits for the test to complete it explicitly.
 *
 * No device, key or captured traffic is involved.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "fpc_disum_diagnostics.h"
#include "fpc_disum_proto.h"
#include "fpc_disum_session.h"
#include "fpc_disum_tls.h"
#include "fpc-disum-corpus.h"

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
#define EVT_UNREVIEWED 0x42

#define RESERVED_ARM_RESULT 0x64
#define GET_STATE_CAPACITY 72
#define GET_STATE_LEN 72
#define GET_TLS_KEY_CAPACITY 1000
#define IMAGE_PLANE_LEN FPC_DISUM_IMAGE_PIXELS
#define APP_MESSAGE_LEN FPC_DISUM_IMAGE_TOTAL_LEN

/* A session that polls without progress and without a window is a bug; fail loudly
 * instead of looping forever. */
#define MAX_EMPTY_READS 64

/* --------------------------------------------------------------- fake timer */

typedef struct
{
  guint interval_ms;
  GSourceFunc callback;
  gpointer user_data;
} FakeTimeout;

typedef struct
{
  GPtrArray *pending; /* FakeTimeout * */
  guint last_interval_ms;
  guint adds;
} FakeTimer;

static gpointer
fake_timer_add (gpointer context, guint interval_ms, GSourceFunc callback, gpointer user_data)
{
  FakeTimer *timer = context;
  FakeTimeout *timeout = g_new0 (FakeTimeout, 1);

  timeout->interval_ms = interval_ms;
  timeout->callback = callback;
  timeout->user_data = user_data;
  g_ptr_array_add (timer->pending, timeout);
  timer->last_interval_ms = interval_ms;
  timer->adds++;

  return timeout;
}

static void
fake_timer_remove (gpointer context, gpointer handle)
{
  FakeTimer *timer = context;
  FakeTimeout *timeout = handle;

  if (timeout == NULL)
    return;

  g_ptr_array_remove (timer->pending, timeout);
  g_free (timeout);
}

/* Run the oldest pending timeout once, as a main loop would. */
static gboolean
fake_timer_fire (FakeTimer *timer)
{
  FakeTimeout *timeout;
  GSourceFunc callback;
  gpointer user_data;

  if (timer->pending->len == 0)
    return FALSE;

  timeout = g_ptr_array_steal_index (timer->pending, 0);
  callback = timeout->callback;
  user_data = timeout->user_data;
  g_free (timeout);

  callback (user_data);

  return TRUE;
}

/* -------------------------------------------------------------- fake device */

typedef struct
{
  guint32 code;
  guint32 third;
} FakeEvent;

typedef struct
{
  FpcDisumTransportStatus status;
  gsize bytes;
} FakeReadStep;

typedef enum
{
  HOLD_NONE = 0,
  HOLD_OUT,
  HOLD_IN,
  HOLD_READ,
  HOLD_REOPEN,
} FakeHoldKind;

typedef struct
{
  FpcDisumTransport transport;

  FakeTimer timer;
  FpcDisumDiagnostics *diagnostics; /* borrowed by the fake */

  /* Device side of the TLS-PSK channel. */
  SSL_CTX *ctx;
  SSL *ssl;
  BIO *rbio;
  BIO *wbio;
  guint8 psk[FPC_DISUM_PSK_LEN];
  guint8 client_psk[FPC_DISUM_PSK_LEN];
  gboolean tls_established;

  /* Script. */
  gboolean respond_handshake;
  gboolean send_finger_down;
  gboolean send_image;
  gboolean send_app_before_touch;
  gboolean auto_fire_idle;
  gboolean fail_reopen;
  gboolean fail_commit;
  gboolean envelope_corrupt;
  gboolean wrong_psk;
  gboolean fail_abort;
  gboolean fail_bulk_now;
  gboolean arm_io_failure;
  gboolean bump_generation_on_reopen;
  gboolean defer_cancel;
  gsize arm_actual_length;
  guint get_tls_key_no_device;
  gboolean read_no_device_next;
  gboolean read_integrity_next;
  guint8 out_io_request;
  guint8 out_short_request;
  gsize out_short_length;
  guint tls_frame_size; /* TLS stream bytes per EVT_TLS frame; 0 = one frame per flight */
  guint read_chunk;     /* bytes per bulk read; 0 = the endpoint packet size */
  guint32 app_code;
  guint32 app_total_len;
  const guint8 *envelope;
  gsize envelope_len;
  guint8 plane[IMAGE_PLANE_LEN];

  /* Physical identity observed by the session. */
  guint64 generation;
  guint64 handle_id;
  guint generation_bumps;

  /* Explicit read schedule: each entry is one completion. */
  GQueue *read_script; /* FakeReadStep * */

  /* Observed device state. */
  gsize delivered;       /* bytes handed to the host so far */
  gsize finger_down_end; /* stream offset just past the touch frame, 0 if none */
  gboolean needs_reopen;
  gboolean reopened;
  gboolean expect_seen_before_init;
  gboolean reopen_in_flight;
  gboolean armed;
  gboolean finger_down_seen;
  gboolean stale_handle_used;
  gboolean get_img_without_touch;
  gboolean arm_before_handshake;
  gboolean rebind_open;
  guint tls_frames;
  guint init_count;
  guint commit_count;

  /* Recording. */
  GPtrArray *log; /* gchar * */
  guint bulk_reads;
  guint empty_reads;
  GQueue *pre_handshake_events; /* delivered just before the client hello */
  GQueue *arm_events;           /* delivered on ARM, just before the finger-down event */
  GQueue *img_events;

  /* Outgoing framed bytes. */
  GByteArray *out;

  /* The operation the device currently keeps in flight. */
  guint hold_counter;
  guint hold_at;       /* 1-based operation index to hold; 0 = never */
  guint8 hold_request; /* command id to hold once; 0 = never */
  FakeHoldKind held_kind;
  guint64 held_op_id;
  gsize held_requested_length;
  FpcDisumTransportDoneFunc held_done;
  FpcDisumTransportDataFunc held_data_done;
  gpointer held_user_data;
  gboolean cancel_deferred;

  /* Completion records. */
  guint64 current_op_id;
} FakeDevice;

/* ------------------------------------------------------- fake diagnostics */

static guint64
fake_begin_op (FakeDevice *dev, gboolean is_out, gboolean control, guint8 request, gsize requested)
{
  FpcDisumDiagnosticRecord record = { 0 };
  guint64 op_id;

  if (dev->diagnostics == NULL)
    return 0;

  op_id = fpc_disum_diagnostics_next_op_id (dev->diagnostics);
  dev->current_op_id = op_id;

  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_SUBMIT;
  record.op_id = op_id;
  record.direction_valid = TRUE;
  record.direction
      = is_out ? FPC_DISUM_DIAGNOSTIC_DIRECTION_OUT : FPC_DISUM_DIAGNOSTIC_DIRECTION_IN;
  record.control_valid = control;
  if (control)
    {
      record.bm_request_type = is_out ? 0x40 : 0xc0;
      record.request = request;
      record.index = 0;
    }
  record.requested_length_valid = control;
  record.requested_length = requested;
  record.submitted_valid = TRUE;
  record.submitted = TRUE;

  fpc_disum_diagnostics_emit (dev->diagnostics, &record);
  return op_id;
}

static void
fake_end_op (FakeDevice *dev, guint64 op_id, FpcDisumTransportStatus status, gsize actual,
    gboolean actual_known)
{
  FpcDisumDiagnosticRecord record = { 0 };

  if (dev->diagnostics == NULL || op_id == 0)
    return;

  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_COMPLETE;
  record.op_id = op_id;
  record.status_valid = TRUE;
  record.status = status;
  record.actual_length_known = actual_known;
  record.actual_length = actual;
  record.native_valid = TRUE;
  record.native_domain = FPC_DISUM_DIAGNOSTIC_NATIVE_LIBUSB;
  record.native_status = status == FPC_DISUM_TRANSPORT_OK ? 0 : -1;
  record.submitted_valid = TRUE;
  record.submitted = TRUE;

  fpc_disum_diagnostics_emit (dev->diagnostics, &record);
}

/* Keep the operation when it was chosen (by index or by request), otherwise let
 * it complete.  Each knob holds a single operation. */
static gboolean
fake_should_hold (FakeDevice *dev, guint8 request)
{
  if (dev->hold_request != 0 && dev->hold_request == request)
    {
      dev->hold_request = 0;
      return TRUE;
    }

  if (dev->hold_at != 0 && ++dev->hold_counter == dev->hold_at)
    {
      dev->hold_at = 0;
      return TRUE;
    }

  return FALSE;
}

static void
fake_release_held (FakeDevice *dev)
{
  dev->held_kind = HOLD_NONE;
  dev->held_done = NULL;
  dev->held_data_done = NULL;
  dev->held_user_data = NULL;
  dev->held_op_id = 0;
  dev->held_requested_length = 0;
}

static void
fake_hold_control (FakeDevice *dev, FakeHoldKind kind, FpcDisumTransportDoneFunc done,
    gpointer user_data, gsize requested)
{
  dev->held_kind = kind;
  dev->held_done = done;
  dev->held_user_data = user_data;
  dev->held_op_id = dev->current_op_id;
  dev->held_requested_length = requested;
}

static void
fake_hold_data (FakeDevice *dev, FpcDisumTransportDataFunc done, gpointer user_data)
{
  dev->held_kind = HOLD_READ;
  dev->held_data_done = done;
  dev->held_user_data = user_data;
  dev->held_op_id = dev->current_op_id;
}

static void fake_log (FakeDevice *dev, const gchar *format, ...) G_GNUC_PRINTF (2, 3);

static void
fake_log (FakeDevice *dev, const gchar *format, ...)
{
  va_list args;
  gchar *line;

  va_start (args, format);
  line = g_strdup_vprintf (format, args);
  va_end (args);

  g_ptr_array_add (dev->log, line);
}

static const gchar *
fake_log_entry (FakeDevice *dev, guint index)
{
  if (index >= dev->log->len)
    return "<missing>";

  return g_ptr_array_index (dev->log, index);
}

static gchar *
payload_hex (const guint8 *data, gsize len)
{
  GString *hex = g_string_new (NULL);

  for (gsize i = 0; i < len; i++)
    g_string_append_printf (hex, "%02x", data[i]);

  return g_string_free (hex, FALSE);
}

/* Compare the last operations against @expected. */
static void
assert_log_tail (FakeDevice *dev, const gchar *const *expected)
{
  guint n = g_strv_length ((gchar **)expected);

  g_assert_cmpuint (dev->log->len, >=, n);
  for (guint i = 0; i < n; i++)
    g_assert_cmpstr (fake_log_entry (dev, dev->log->len - n + i), ==, expected[i]);
}

static gboolean
log_contains (FakeDevice *dev, const gchar *prefix)
{
  for (guint i = 0; i < dev->log->len; i++)
    {
      if (g_str_has_prefix (fake_log_entry (dev, i), prefix))
        return TRUE;
    }

  return FALSE;
}

static gint
log_index (FakeDevice *dev, const gchar *prefix)
{
  for (guint i = 0; i < dev->log->len; i++)
    {
      if (g_str_has_prefix (fake_log_entry (dev, i), prefix))
        return (gint)i;
    }

  return -1;
}

static guint
log_count (FakeDevice *dev, const gchar *prefix)
{
  guint count = 0;

  for (guint i = 0; i < dev->log->len; i++)
    {
      if (g_str_has_prefix (fake_log_entry (dev, i), prefix))
        count++;
    }

  return count;
}

static void
fake_push_frame (FakeDevice *dev, guint32 code, guint32 third, const guint8 *payload, gsize len)
{
  guint8 header[FPC_DISUM_FRAME_HEADER_LEN];
  guint32 total = FPC_DISUM_FRAME_HEADER_LEN + len;

  header[0] = code >> 24;
  header[1] = code >> 16;
  header[2] = code >> 8;
  header[3] = code;
  header[4] = total >> 24;
  header[5] = total >> 16;
  header[6] = total >> 8;
  header[7] = total;
  header[8] = third >> 24;
  header[9] = third >> 16;
  header[10] = third >> 8;
  header[11] = third;

  /* The touch counts as delivered only once its bytes reach the host. */
  if (code == EVT_FINGER_DWN)
    dev->finger_down_end = dev->out->len + total;

  g_byte_array_append (dev->out, header, sizeof header);
  if (len > 0)
    g_byte_array_append (dev->out, payload, len);

  if (code == EVT_TLS)
    dev->tls_frames++;
}

static void
fake_queue_event (GQueue *queue, guint32 code, guint32 third)
{
  FakeEvent *event = g_new0 (FakeEvent, 1);

  event->code = code;
  event->third = third;
  g_queue_push_tail (queue, event);
}

static void
fake_flush_events (FakeDevice *dev, GQueue *queue)
{
  FakeEvent *event;

  while ((event = g_queue_pop_head (queue)) != NULL)
    {
      fake_push_frame (dev, event->code, event->third, NULL, 0);
      g_free (event);
    }
}

static void
fake_queue_read (FakeDevice *dev, FpcDisumTransportStatus status, gsize bytes)
{
  FakeReadStep *step = g_new0 (FakeReadStep, 1);

  step->status = status;
  step->bytes = bytes;
  g_queue_push_tail (dev->read_script, step);
}

/* Wrap TLS bytes the device produced into EVT_TLS frames (SPEC §4.4/§8). */
static void
fake_frame_tls (FakeDevice *dev, const guint8 *data, gsize len)
{
  gsize chunk = dev->tls_frame_size > 0 ? dev->tls_frame_size : len;

  for (gsize offset = 0; offset < len; offset += chunk)
    {
      gsize n = MIN (chunk, len - offset);

      fake_push_frame (dev, EVT_TLS, 0, data + offset, n);
    }
}

static void
fake_drain_tls (FakeDevice *dev)
{
  GByteArray *records = g_byte_array_new ();
  guint8 buffer[4096];
  int n;

  while ((n = BIO_read (dev->wbio, buffer, sizeof buffer)) > 0)
    g_byte_array_append (records, buffer, n);

  if (records->len > 0)
    fake_frame_tls (dev, records->data, records->len);

  g_byte_array_free (records, TRUE);
}

static void
fake_run_handshake (FakeDevice *dev)
{
  if (SSL_do_handshake (dev->ssl) == 1)
    dev->tls_established = TRUE;

  fake_drain_tls (dev);
}

static void
fake_feed_tls (FakeDevice *dev, const guint8 *data, gsize len)
{
  g_assert_cmpint (BIO_write (dev->rbio, data, (int)len), ==, (int)len);
  fake_run_handshake (dev);
}

/* The application message of SPEC §5: a 12 B header, a 12 B inner header and the
 * pixel plane, padded or truncated to the scripted total length. */
static GByteArray *
fake_app_message (FakeDevice *dev)
{
  GByteArray *message = g_byte_array_new ();
  guint8 header[FPC_DISUM_FRAME_HEADER_LEN];
  guint8 inner[FPC_DISUM_FRAME_HEADER_LEN] = { 0x00, 0x00, 0x2f, 0x11, 0, 0, 0, 0, 0, 0, 0, 0 };
  guint32 total = dev->app_total_len;

  header[0] = dev->app_code >> 24;
  header[1] = dev->app_code >> 16;
  header[2] = dev->app_code >> 8;
  header[3] = dev->app_code;
  header[4] = total >> 24;
  header[5] = total >> 16;
  header[6] = total >> 8;
  header[7] = total;
  header[8] = 0;
  header[9] = 0;
  header[10] = 0;
  header[11] = 0;

  g_byte_array_append (message, header, sizeof header);

  if (total >= 2 * FPC_DISUM_FRAME_HEADER_LEN)
    {
      g_byte_array_append (message, inner, sizeof inner);
      g_byte_array_set_size (message, total);
      memcpy (message->data + 2 * FPC_DISUM_FRAME_HEADER_LEN, dev->plane,
          MIN (sizeof dev->plane, total - 2 * FPC_DISUM_FRAME_HEADER_LEN));
    }

  return message;
}

static void
fake_send_image (FakeDevice *dev)
{
  GByteArray *message = fake_app_message (dev);

  g_assert_cmpint (SSL_write (dev->ssl, message->data, (int)message->len), ==, (int)message->len);
  g_byte_array_free (message, TRUE);
  fake_drain_tls (dev);
}

/* ---------------------------------------------------- completion helpers */

static void
fake_done_out (FakeDevice *dev, FpcDisumTransportDoneFunc done, gpointer user_data,
    FpcDisumTransportStatus status, gsize actual)
{
  fake_end_op (dev, dev->current_op_id, status, actual, TRUE);
  done (&dev->transport, status, actual, user_data);
}

static void
fake_done_data (FakeDevice *dev, FpcDisumTransportDataFunc done, gpointer user_data,
    FpcDisumTransportStatus status, const guint8 *data, gsize len)
{
  fake_end_op (dev, dev->current_op_id, status, len, TRUE);
  done (&dev->transport, status, data, len, user_data);
}

/* ------------------------------------------------------- physical identity */

static void
fake_identity (FpcDisumTransport *transport, FpcDisumTransportIdentity *out)
{
  FakeDevice *dev = (FakeDevice *)transport;

  memset (out, 0, sizeof *out);
  out->generation = dev->generation;
  out->handle_id = dev->handle_id;
  out->bus = 1;
  out->port_count = 2;
  out->ports[0] = 2;
  out->ports[1] = 3;
  out->vid = 0x10a5;
  out->pid = 0x9201;
  out->bcd_device = 0x0250;
  out->address = 0;
}

static FpcDisumTransportStatus
fake_init_rebind (FpcDisumTransport *transport, FpcDisumInitRebindAction action, gint64 deadline_us)
{
  FakeDevice *dev = (FakeDevice *)transport;

  switch (action)
    {
    case FPC_DISUM_INIT_REBIND_BEGIN:
      fake_log (dev, "REBIND BEGIN");
      dev->rebind_open = TRUE;
      dev->init_count++;
      break;
    case FPC_DISUM_INIT_REBIND_ALLOW:
      fake_log (dev, "REBIND ALLOW");
      if (!dev->rebind_open)
        return FPC_DISUM_TRANSPORT_ERROR_IO;
      break;
    case FPC_DISUM_INIT_REBIND_COMMIT:
      fake_log (dev, "REBIND COMMIT");
      if (!dev->rebind_open)
        return FPC_DISUM_TRANSPORT_ERROR_IO;
      if (dev->fail_commit)
        return FPC_DISUM_TRANSPORT_ERROR_IO;
      dev->rebind_open = FALSE;
      dev->commit_count++;
      break;
    case FPC_DISUM_INIT_REBIND_ABORT:
      fake_log (dev, "REBIND ABORT");
      dev->rebind_open = FALSE;
      break;
    }

  return FPC_DISUM_TRANSPORT_OK;
}

/* ------------------------------------------------------------ transport impl */

static void
fake_control_out (FpcDisumTransport *transport, guint8 request, guint16 value, const guint8 *data,
    gsize len, FpcDisumTransportDoneFunc done, gpointer user_data)
{
  FakeDevice *dev = (FakeDevice *)transport;
  gchar *hex = payload_hex (data, len);
  gsize actual = len;
  FpcDisumTransportStatus status = FPC_DISUM_TRANSPORT_OK;

  fake_begin_op (dev, TRUE, TRUE, request, len);
  fake_log (dev, "OUT %02x v=%04x len=%zu p=%s", request, value, len, hex);
  g_free (hex);

  /* After CMD_INIT every transfer on the old handle fails until it is swapped
   * (SPEC §6 step 4). */
  if (dev->needs_reopen)
    {
      dev->stale_handle_used = TRUE;
      status = FPC_DISUM_TRANSPORT_ERROR_IO;
      actual = 0;
      fake_done_out (dev, done, user_data, status, actual);
      return;
    }

  if (dev->out_io_request == request)
    {
      status = FPC_DISUM_TRANSPORT_ERROR_IO;
      actual = 0;
      fake_done_out (dev, done, user_data, status, actual);
      return;
    }

  if (dev->out_short_request == request)
    actual = MIN (dev->out_short_length, len);

  switch (request)
    {
    case CMD_INDICATE_S_STATE:
    case CMD_SESSION_OFF:
      break;
    case CMD_INIT:
      /* The post-reset absence has to be announced before the command goes out. */
      dev->expect_seen_before_init = dev->rebind_open;
      dev->needs_reopen = TRUE;
      break;
    case CMD_ARM:
      if (!dev->tls_established)
        dev->arm_before_handshake = TRUE;

      if (dev->arm_io_failure)
        {
          status = FPC_DISUM_TRANSPORT_ERROR_IO;
          actual = 4;
          break;
        }

      if (dev->arm_actual_length != 4)
        {
          /* A short completion is never an accepted ARM. */
          actual = dev->arm_actual_length;
          break;
        }

      if (len == 4 && data[0] == 0x12)
        {
          dev->armed = FALSE;
        }
      else
        {
          dev->armed = TRUE;
          fake_flush_events (dev, dev->arm_events);
          if (dev->send_app_before_touch)
            fake_send_image (dev);
          else if (dev->send_finger_down)
            fake_push_frame (dev, EVT_FINGER_DWN, 0, NULL, 0);
        }
      break;
    case CMD_ABORT:
      if (dev->fail_abort)
        {
          status = FPC_DISUM_TRANSPORT_ERROR_IO;
          actual = 0;
        }
      break;
    case CMD_TLS_INIT:
      if (dev->respond_handshake)
        fake_run_handshake (dev);
      break;
    case CMD_TLS_DATA:
      if (dev->respond_handshake)
        fake_feed_tls (dev, data, len);
      break;
    case CMD_GET_IMG:
      if (!dev->finger_down_seen)
        dev->get_img_without_touch = TRUE;
      fake_flush_events (dev, dev->img_events);
      /* A request that did not follow the touch is never answered (SPEC §4.5). */
      if (dev->send_image && dev->finger_down_seen)
        fake_send_image (dev);
      break;
    default:
      break;
    }

  if (status != FPC_DISUM_TRANSPORT_OK)
    {
      fake_done_out (dev, done, user_data, status, actual);
      return;
    }

  if (fake_should_hold (dev, request))
    {
      /* The submitted op id is kept until the test completes the operation. */
      fake_hold_control (dev, HOLD_OUT, done, user_data, len);
      return;
    }

  dev->reopen_in_flight = FALSE;
  fake_done_out (dev, done, user_data, FPC_DISUM_TRANSPORT_OK, actual);
}

static void
fake_control_in (FpcDisumTransport *transport, guint8 request, guint16 value, gsize requested,
    FpcDisumTransportDataFunc done, gpointer user_data)
{
  FakeDevice *dev = (FakeDevice *)transport;
  guint8 state[GET_STATE_LEN] = { 0 };

  fake_begin_op (dev, FALSE, TRUE, request, requested);
  fake_log (dev, "IN %02x v=%04x req=%zu", request, value, requested);

  if (dev->needs_reopen)
    {
      dev->stale_handle_used = TRUE;
      fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
      return;
    }

  switch (request)
    {
    case CMD_GET_STATE:
      g_assert_cmpuint (requested, ==, GET_STATE_CAPACITY);
      if (fake_should_hold (dev, request))
        {
          fake_hold_data (dev, done, user_data);
          return;
        }
      fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_OK, state, sizeof state);
      break;
    case CMD_GET_TLS_KEY:
      {
        guint8 envelope[FPC_DISUM_ENVELOPE_LEN];

        g_assert_cmpuint (requested, ==, GET_TLS_KEY_CAPACITY);

        if (dev->get_tls_key_no_device > 0)
          {
            dev->get_tls_key_no_device--;
            fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE, NULL, 0);
            return;
          }

        g_assert_cmpuint (dev->envelope_len, ==, sizeof envelope);
        memcpy (envelope, dev->envelope, sizeof envelope);
        if (dev->envelope_corrupt)
          envelope[FPC_DISUM_ENVELOPE_LEN - 1] ^= 0xff;

        fake_flush_events (dev, dev->pre_handshake_events);
        if (fake_should_hold (dev, request))
          {
            fake_hold_data (dev, done, user_data);
            return;
          }
        fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_OK, envelope, sizeof envelope);
      }
      break;
    default:
      fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_OK, NULL, 0);
      break;
    }
}

static void
fake_bulk_read (FpcDisumTransport *transport, FpcDisumTransportDataFunc done, gpointer user_data)
{
  FakeDevice *dev = (FakeDevice *)transport;
  guint8 buffer[64];
  gsize chunk;

  fake_begin_op (dev, FALSE, FALSE, 0, 0);
  dev->bulk_reads++;

  if (dev->read_no_device_next)
    {
      dev->read_no_device_next = FALSE;
      fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE, NULL, 0);
      return;
    }

  if (dev->read_integrity_next)
    {
      dev->read_integrity_next = FALSE;
      fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN, NULL, 0);
      return;
    }

  if (dev->fail_bulk_now)
    {
      dev->fail_bulk_now = FALSE;
      fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
      return;
    }

  if (g_queue_get_length (dev->read_script) > 0)
    {
      FakeReadStep *step = g_queue_pop_head (dev->read_script);
      gsize n = MIN (step->bytes, dev->out->len);
      FpcDisumTransportStatus status = step->status;

      if (n > 0)
        {
          memcpy (buffer, dev->out->data, n);
          g_byte_array_remove_range (dev->out, 0, n);
          dev->delivered += n;
          if (dev->finger_down_end > 0 && dev->delivered >= dev->finger_down_end)
            dev->finger_down_seen = TRUE;
        }

      fake_done_data (dev, done, user_data, status, n > 0 ? buffer : NULL, n);
      g_free (step);
      return;
    }

  if (dev->out->len == 0)
    {
      dev->empty_reads++;
      if (dev->empty_reads > MAX_EMPTY_READS)
        g_error (
            "the session polled the reader %u times with no data and no window", dev->empty_reads);

      if (!dev->auto_fire_idle)
        {
          /* The device stays silent: keep the read in flight until the caller
           * cancels it or a test completes it. */
          fake_hold_data (dev, done, user_data);
          return;
        }

      /* Let the pending window expire, as a main loop would. */
      fake_timer_fire (&dev->timer);
      fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_OK, NULL, 0);
      return;
    }

  dev->empty_reads = 0;
  chunk = dev->read_chunk > 0 ? dev->read_chunk : sizeof buffer;
  chunk = MIN (chunk, MIN (dev->out->len, sizeof buffer));

  memcpy (buffer, dev->out->data, chunk);
  g_byte_array_remove_range (dev->out, 0, chunk);

  dev->delivered += chunk;
  if (dev->finger_down_end > 0 && dev->delivered >= dev->finger_down_end)
    dev->finger_down_seen = TRUE;

  fake_done_data (dev, done, user_data, FPC_DISUM_TRANSPORT_OK, buffer, chunk);
}

static void
fake_reopen (FpcDisumTransport *transport, gint64 deadline_us, FpcDisumTransportDoneFunc done,
    gpointer user_data)
{
  FakeDevice *dev = (FakeDevice *)transport;

  fake_begin_op (dev, FALSE, FALSE, 0, 0);
  fake_log (dev, "REOPEN");
  dev->reopen_in_flight = TRUE;

  if (dev->fail_reopen)
    {
      dev->reopen_in_flight = FALSE;
      fake_done_out (dev, done, user_data, FPC_DISUM_TRANSPORT_ERROR_IO, 0);
      return;
    }

  /* A new handle.  The current configuration is read first and SET_CONFIGURATION is
   * skipped because the target configuration is already active (SPEC §16.2). */
  fake_log (dev, "CONFIG CURRENT");
  dev->needs_reopen = FALSE;
  dev->reopened = TRUE;
  dev->handle_id++;
  if (dev->bump_generation_on_reopen)
    {
      dev->generation++;
      dev->generation_bumps++;
    }

  /* The handle swap has no request id; only the index knob can hold it. */
  if (fake_should_hold (dev, 0))
    {
      fake_hold_control (dev, HOLD_REOPEN, done, user_data, 0);
      return;
    }

  fake_done_out (dev, done, user_data, FPC_DISUM_TRANSPORT_OK, 0);
}

static void
fake_cancel (FpcDisumTransport *transport)
{
  FakeDevice *dev = (FakeDevice *)transport;

  if (dev->held_kind == HOLD_NONE)
    return;

  /* A delayed cancel is only a request: the I/O has not completed yet. */
  if (dev->defer_cancel)
    {
      dev->cancel_deferred = TRUE;
      return;
    }

  {
    FakeHoldKind kind = dev->held_kind;
    FpcDisumTransportDoneFunc done = dev->held_done;
    FpcDisumTransportDataFunc data_done = dev->held_data_done;
    gpointer user_data = dev->held_user_data;
    guint64 op_id = dev->held_op_id;

    fake_release_held (dev);
    dev->cancel_deferred = FALSE;

    if (kind == HOLD_READ)
      {
        fake_end_op (dev, op_id, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, 0, TRUE);
        data_done (&dev->transport, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, NULL, 0, user_data);
      }
    else
      {
        fake_end_op (dev, op_id, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, 0, TRUE);
        done (&dev->transport, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, 0, user_data);
      }
  }
}

/* Complete the held operation from the test, as the transport eventually would. */
static void
fake_complete_held_control (FakeDevice *dev, FpcDisumTransportStatus status, gsize actual)
{
  FpcDisumTransportDoneFunc done = dev->held_done;
  gpointer user_data = dev->held_user_data;
  guint64 op_id = dev->held_op_id;

  g_assert_cmpint (dev->held_kind, !=, HOLD_NONE);
  g_assert_nonnull (done);

  fake_release_held (dev);
  dev->cancel_deferred = FALSE;

  fake_end_op (dev, op_id, status, actual, TRUE);
  done (&dev->transport, status, actual, user_data);
}

static void
fake_complete_held_data (
    FakeDevice *dev, FpcDisumTransportStatus status, const guint8 *data, gsize len)
{
  FpcDisumTransportDataFunc done = dev->held_data_done;
  gpointer user_data = dev->held_user_data;
  guint64 op_id = dev->held_op_id;

  g_assert_cmpint (dev->held_kind, !=, HOLD_NONE);
  g_assert_nonnull (done);

  fake_release_held (dev);
  dev->cancel_deferred = FALSE;

  fake_end_op (dev, op_id, status, len, TRUE);
  done (&dev->transport, status, data, len, user_data);
}

/* Deliver everything the device has queued as one completion of the held read. */
static void
fake_complete_held_from_out (FakeDevice *dev, FpcDisumTransportStatus status)
{
  guint8 *bytes;
  gsize len = dev->out->len;

  g_assert_cmpint (dev->held_kind, ==, HOLD_READ);

  bytes = g_memdup2 (dev->out->data, len);
  g_byte_array_set_size (dev->out, 0);

  fake_complete_held_data (dev, status, bytes, len);
  g_free (bytes);
}

/* ------------------------------------------------------------ device set-up */

static FakeDevice *fake_device_new_standalone (void);

static const FpcDisumCorpusEnvelopeVector *
accepted_envelope (void)
{
  for (gsize i = 0; i < fpc_disum_corpus_n_envelope_vectors; i++)
    {
      if (fpc_disum_corpus_envelope_vectors[i].accepted)
        return &fpc_disum_corpus_envelope_vectors[i];
    }

  g_assert_not_reached ();
  return NULL;
}

static unsigned int
fake_client_psk_callback (SSL *ssl, const char *hint, char *identity, unsigned int max_identity_len,
    unsigned char *psk, unsigned int max_psk_len)
{
  FakeDevice *dev = SSL_get_app_data (ssl);
  const gchar *name = "Disum PSK";
  gsize name_len = strlen (name) + 1;

  if (max_identity_len < name_len || max_psk_len < FPC_DISUM_PSK_LEN)
    return 0;

  memcpy (identity, name, name_len);
  memcpy (psk, dev->client_psk, FPC_DISUM_PSK_LEN);

  return FPC_DISUM_PSK_LEN;
}

static FakeDevice *
fake_device_new (const FpcDisumCorpusEnvelopeVector *vector)
{
  FakeDevice *dev = g_new0 (FakeDevice, 1);

  dev->transport.control_out = fake_control_out;
  dev->transport.control_in = fake_control_in;
  dev->transport.bulk_read = fake_bulk_read;
  dev->transport.reopen = fake_reopen;
  dev->transport.init_rebind = fake_init_rebind;
  dev->transport.identity = fake_identity;
  dev->transport.cancel = fake_cancel;

  dev->timer.pending = g_ptr_array_new ();
  dev->log = g_ptr_array_new_with_free_func (g_free);
  dev->pre_handshake_events = g_queue_new ();
  dev->arm_events = g_queue_new ();
  dev->img_events = g_queue_new ();
  dev->read_script = g_queue_new ();
  dev->out = g_byte_array_new ();
  dev->respond_handshake = TRUE;
  dev->send_finger_down = TRUE;
  dev->send_image = TRUE;
  dev->auto_fire_idle = TRUE;
  dev->arm_actual_length = 4;
  dev->bump_generation_on_reopen = TRUE;
  dev->app_code = FPC_DISUM_IMAGE_CODE;
  dev->app_total_len = APP_MESSAGE_LEN;
  dev->generation = 1;
  dev->handle_id = 1;

  for (gsize i = 0; i < sizeof dev->plane; i++)
    dev->plane[i] = (guint8)(i * 31 + 7);

  memcpy (dev->psk, vector->psk, FPC_DISUM_PSK_LEN);
  memcpy (dev->client_psk, vector->psk, FPC_DISUM_PSK_LEN);
  dev->envelope = vector->envelope;
  dev->envelope_len = vector->envelope_len;

  dev->ctx = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (dev->ctx);
  g_assert_cmpint (SSL_CTX_set_min_proto_version (dev->ctx, TLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_set_max_proto_version (dev->ctx, TLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_set_cipher_list (dev->ctx, "PSK-AES128-CBC-SHA256"), ==, 1);
  SSL_CTX_set_psk_client_callback (dev->ctx, fake_client_psk_callback);

  dev->ssl = SSL_new (dev->ctx);
  g_assert_nonnull (dev->ssl);
  dev->rbio = BIO_new (BIO_s_mem ());
  dev->wbio = BIO_new (BIO_s_mem ());
  g_assert_nonnull (dev->rbio);
  g_assert_nonnull (dev->wbio);
  BIO_set_mem_eof_return (dev->rbio, -1);

  /* The SSL object owns the BIOs from here on. */
  SSL_set_bio (dev->ssl, dev->rbio, dev->wbio);
  SSL_set_connect_state (dev->ssl);
  SSL_set_app_data (dev->ssl, dev);

  return dev;
}

static void
fake_device_free (FakeDevice *dev)
{
  if (dev == NULL)
    return;

  SSL_free (dev->ssl);
  SSL_CTX_free (dev->ctx);
  g_ptr_array_free (dev->log, TRUE);
  g_queue_free_full (dev->pre_handshake_events, g_free);
  g_queue_free_full (dev->arm_events, g_free);
  g_queue_free_full (dev->img_events, g_free);
  g_queue_free_full (dev->read_script, g_free);
  g_byte_array_free (dev->out, TRUE);
  g_ptr_array_free (dev->timer.pending, TRUE);
  g_free (dev);
}

/* ------------------------------------------------------------------ fixture */

typedef struct
{
  FakeDevice *dev;
  FpcDisumSession *session;
  FpcDisumDiagnostics *diagnostics;
  GPtrArray *records; /* FpcDisumDiagnosticRecord * */
  guint8 image[IMAGE_PLANE_LEN];
  gsize image_len;
  FpcDisumSessionStatus init_status;
  FpcDisumSessionStatus capture_status;
  FpcDisumSessionStatus close_status;
  const gchar *capture_message;
  guint init_calls;
  guint capture_calls;
  guint close_calls;
  guint touch_calls;
  guint arm_calls;
  gsize arm_actual;
  gboolean cancel_on_touch;
} Fixture;

static void
fixture_touch (gpointer user_data)
{
  Fixture *fixture = user_data;

  fixture->touch_calls++;

  if (fixture->cancel_on_touch)
    fpc_disum_session_cancel (fixture->session);
}

static void
fixture_arm (gsize actual_length, gpointer user_data)
{
  Fixture *fixture = user_data;

  fixture->arm_calls++;
  fixture->arm_actual = actual_length;
}

static void
fixture_init_done (FpcDisumSessionStatus status, const gchar *message, gpointer user_data)
{
  Fixture *fixture = user_data;

  fixture->init_status = status;
  fixture->init_calls++;
}

static void
fixture_capture_done (const FpcDisumCaptureResult *result, gpointer user_data)
{
  Fixture *fixture = user_data;

  fixture->capture_status = result->status;
  fixture->capture_message = result->message;
  fixture->capture_calls++;

  if (result->image != NULL)
    {
      fixture->image_len = MIN (result->image_len, sizeof fixture->image);
      memcpy (fixture->image, result->image, fixture->image_len);
    }
}

static void
fixture_close_done (FpcDisumSessionStatus status, const gchar *message, gpointer user_data)
{
  Fixture *fixture = user_data;

  fixture->close_status = status;
  fixture->close_calls++;
}

static void
fixture_diagnostic (const FpcDisumDiagnosticRecord *record, gpointer user_data)
{
  Fixture *fixture = user_data;
  FpcDisumDiagnosticRecord *copy = g_new0 (FpcDisumDiagnosticRecord, 1);

  *copy = *record;
  g_ptr_array_add (fixture->records, copy);
}

static void
fixture_setup (Fixture *fixture)
{
  const FpcDisumCorpusEnvelopeVector *vector = accepted_envelope ();
  FpcDisumTimeoutSource source = { 0 };

  memset (fixture, 0, sizeof *fixture);
  fixture->dev = fake_device_new (vector);
  fixture->diagnostics = fpc_disum_diagnostics_new ();
  fixture->records = g_ptr_array_new_with_free_func (g_free);
  fixture->dev->diagnostics = fixture->diagnostics;
  fpc_disum_diagnostics_set_observer (fixture->diagnostics, fixture_diagnostic, fixture);

  fixture->session = fpc_disum_session_new (&fixture->dev->transport);
  g_assert_nonnull (fixture->session);

  source.add = fake_timer_add;
  source.remove = fake_timer_remove;
  source.context = &fixture->dev->timer;
  fpc_disum_session_set_timeout_source (fixture->session, &source);
  fpc_disum_session_set_touch_handler (fixture->session, fixture_touch, fixture);
  fpc_disum_session_set_arm_complete_handler (fixture->session, fixture_arm, fixture);
  fpc_disum_session_set_diagnostics (fixture->session, fixture->diagnostics);
}

static void
fixture_capture (Fixture *fixture)
{
  fpc_disum_session_capture_async (fixture->session, fixture_capture_done, fixture);
}

static void
fixture_close (Fixture *fixture)
{
  fpc_disum_session_close_async (fixture->session, fixture_close_done, fixture);
}

static void
fixture_teardown (Fixture *fixture)
{
  /* Every session is closed before it is released, as the device glue will do. */
  if (fixture->close_calls == 0)
    fixture_close (fixture);

  g_assert_cmpuint (fixture->dev->held_kind, ==, HOLD_NONE);
  fpc_disum_session_free (fixture->session);
  fake_device_free (fixture->dev);
  g_ptr_array_free (fixture->records, TRUE);
  fpc_disum_diagnostics_unref (fixture->diagnostics);
}

/* Run the initialisation and require it to succeed. */
static void
fixture_init_ok (Fixture *fixture)
{
  fpc_disum_session_init_async (fixture->session, fixture_init_done, fixture);

  g_assert_cmpint (fixture->init_status, ==, FPC_DISUM_SESSION_OK);
  g_assert_cmpuint (fixture->init_calls, ==, 1);
}

static void
assert_image (Fixture *fixture)
{
  g_assert_cmpuint (fixture->image_len, ==, IMAGE_PLANE_LEN);
  g_assert_cmpmem (fixture->image, fixture->image_len, fixture->dev->plane, IMAGE_PLANE_LEN);
}

/* Index of the first event with @code, or -1. */
static gint
find_event (FpcDisumSession *session, guint32 code)
{
  for (gsize i = 0; i < fpc_disum_session_event_count (session); i++)
    {
      if (fpc_disum_session_event (session, i)->code == code)
        return (gint)i;
    }

  return -1;
}

static FpcDisumDiagnosticRecord *
fixture_find_cleanup (Fixture *fixture, guint16 request)
{
  for (guint i = 0; i < fixture->records->len; i++)
    {
      FpcDisumDiagnosticRecord *record = g_ptr_array_index (fixture->records, i);

      if (record->kind == FPC_DISUM_DIAGNOSTIC_KIND_CLEANUP && record->request == request)
        return record;
    }

  return NULL;
}

static FpcDisumDiagnosticRecord *
fixture_last_round (Fixture *fixture)
{
  FpcDisumDiagnosticRecord *last = NULL;

  for (guint i = 0; i < fixture->records->len; i++)
    {
      FpcDisumDiagnosticRecord *record = g_ptr_array_index (fixture->records, i);

      if (record->kind == FPC_DISUM_DIAGNOSTIC_KIND_ROUND_RESULT)
        last = record;
    }

  return last;
}

/* --------------------------------------------------------------- test cases */

static void
test_init_command_order (void)
{
  Fixture fixture;

  fixture_setup (&fixture);

  /* The device reports the initialisation result while the key is read; it is a
   * notification and must not gate the sequence (SPEC §6). */
  fake_queue_event (fixture.dev->pre_handshake_events, EVT_INIT_RESULT, 0);

  fixture_init_ok (&fixture);

  g_assert_true (fixture.dev->reopened);
  g_assert_true (fixture.dev->tls_established);
  g_assert_false (fixture.dev->stale_handle_used);
  g_assert_false (fixture.dev->arm_before_handshake);

  /* The reclaim transaction wraps the command: BEGIN before CMD_INIT, ALLOW on
   * its successful 4 B completion, COMMIT before TLS_INIT. */
  g_assert_cmpint (log_index (fixture.dev, "REBIND BEGIN"), ==, 2);
  g_assert_cmpint (log_index (fixture.dev, "REBIND ALLOW"), ==, 4);
  g_assert_cmpint (log_index (fixture.dev, "REBIND COMMIT"), ==, 8);
  g_assert_cmpint (log_index (fixture.dev, "OUT 01"), ==, 3);
  g_assert_cmpint (log_index (fixture.dev, "OUT 05"), ==, 9);
  g_assert_cmpuint (fixture.dev->init_count, ==, 1);
  g_assert_cmpuint (fixture.dev->commit_count, ==, 1);

  g_assert_cmpstr (fake_log_entry (fixture.dev, 0), ==, "OUT 08 v=0010 len=0 p=");
  g_assert_cmpstr (fake_log_entry (fixture.dev, 1), ==, "IN 50 v=0000 req=72");

  /* Everything after TLS_INIT is a TLS_DATA fragment. */
  for (guint i = 10; i < fixture.dev->log->len; i++)
    g_assert_true (g_str_has_prefix (fake_log_entry (fixture.dev, i), "OUT 06 v=0001 len="));

  /* The init result is recorded and nothing else is: it is a notification and it
   * does not gate the sequence (SPEC §6). */
  g_assert_cmpuint (fpc_disum_session_event_count (fixture.session), ==, 1);
  g_assert_cmpuint (fpc_disum_session_event (fixture.session, 0)->code, ==, EVT_INIT_RESULT);
  g_assert_cmpuint (
      fpc_disum_session_event (fixture.session, 0)->total_len, ==, FPC_DISUM_FRAME_HEADER_LEN);

  fixture_teardown (&fixture);
}

static void
test_init_handshake_timeout (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->respond_handshake = FALSE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_TLS);
  g_assert_cmpuint (fixture.init_calls, ==, 1);
  /* The handshake made no progress within the reviewed window (SPEC §16.2). */
  g_assert_cmpuint (fixture.dev->timer.last_interval_ms, ==, FPC_DISUM_HANDSHAKE_TIMEOUT_MS);
  g_assert_false (log_contains (fixture.dev, "OUT 02"));

  fixture_close (&fixture);
  {
    static const gchar *const tail[] = {
      "OUT 03 v=0000 len=0 p=",
      "OUT 12 v=0000 len=0 p=",
      NULL,
    };

    assert_log_tail (fixture.dev, tail);
  }
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_TLS);

  fixture_teardown (&fixture);
}

static void
test_init_envelope_rejected (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->envelope_corrupt = TRUE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_ENVELOPE);
  g_assert_false (log_contains (fixture.dev, "OUT 05"));
  g_assert_false (log_contains (fixture.dev, "OUT 06"));
  /* The transaction opened for CMD_INIT is aborted; it was never committed. */
  g_assert_true (log_contains (fixture.dev, "REBIND ABORT"));
  g_assert_false (log_contains (fixture.dev, "REBIND COMMIT"));

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_ENVELOPE);

  fixture_teardown (&fixture);
}

static void
test_init_reopen_failure (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->fail_reopen = TRUE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_true (log_contains (fixture.dev, "REOPEN"));
  g_assert_false (log_contains (fixture.dev, "IN 0b"));
  g_assert_true (log_contains (fixture.dev, "REBIND ABORT"));
  g_assert_false (log_contains (fixture.dev, "REBIND COMMIT"));

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_IO);

  fixture_teardown (&fixture);
}

static void
test_init_commit_failure (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->fail_commit = TRUE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_true (log_contains (fixture.dev, "REBIND COMMIT"));
  g_assert_false (log_contains (fixture.dev, "OUT 05"));
  g_assert_true (log_contains (fixture.dev, "REBIND ABORT"));

  fixture_teardown (&fixture);
}

static void
test_init_short_control_out (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  /* CMD_INIT reports success but returns three of the four bytes. */
  fixture.dev->out_short_request = CMD_INIT;
  fixture.dev->out_short_length = 3;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_false (log_contains (fixture.dev, "REOPEN"));
  g_assert_false (log_contains (fixture.dev, "REBIND ALLOW"));

  fixture_teardown (&fixture);
}

static void
test_init_get_tls_key_retry (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->get_tls_key_no_device = 1;

  fixture_init_ok (&fixture);

  /* The dead old connection is reacquired and only GET_TLS_KEY is re-sent; the
   * initialisation command is never repeated. */
  g_assert_true (log_contains (fixture.dev, "REOPEN"));
  g_assert_cmpuint (log_count (fixture.dev, "OUT 01"), ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "REBIND BEGIN"), ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "IN 0b"), ==, 2);
  g_assert_true (fixture.dev->tls_established);

  fixture_teardown (&fixture);
}

static void
test_init_tls_identity_rejected (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->wrong_psk = TRUE;
  memset (fixture.dev->client_psk, 0x5a, sizeof fixture.dev->client_psk);

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_TLS);
  g_assert_false (fixture.dev->tls_established);
  g_assert_false (log_contains (fixture.dev, "OUT 02"));

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_TLS);

  fixture_teardown (&fixture);
}

static void
test_capture_command_order (void)
{
  Fixture fixture;
  static const gchar *const capture[] = {
    "OUT 02 v=0001 len=4 p=112f1117",
    "OUT 09 v=0000 len=0 p=",
    NULL,
  };
  static const gchar *const cleanup[] = {
    "OUT 02 v=0001 len=4 p=122f1117",
    "OUT 03 v=0000 len=0 p=",
    "OUT 12 v=0000 len=0 p=",
    NULL,
  };

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);
  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_false (fixture.dev->get_img_without_touch);
  assert_image (&fixture);
  /* The touch was reported once, when the device saw it. */
  g_assert_cmpuint (fixture.touch_calls, ==, 1);
  /* ARM completion is signalled once, with its real four bytes. */
  g_assert_cmpuint (fixture.arm_calls, ==, 1);
  g_assert_cmpuint (fixture.arm_actual, ==, 4);

  assert_log_tail (fixture.dev, capture);

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_OK);
  assert_log_tail (fixture.dev, cleanup);

  g_assert_true (fpc_disum_session_was_armed (fixture.session));

  g_assert_nonnull (fixture_find_cleanup (&fixture, CMD_ARM));
  g_assert_nonnull (fixture_find_cleanup (&fixture, CMD_ABORT));
  g_assert_nonnull (fixture_find_cleanup (&fixture, CMD_SESSION_OFF));
  g_assert_cmpstr (fixture_find_cleanup (&fixture, CMD_ABORT)->cleanup_result, ==, "ok");

  fixture_teardown (&fixture);
}

static void
test_multi_capture_five_rounds (void)
{
  Fixture fixture;
  guint init_logs;
  guint tls_init_logs;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  init_logs = log_count (fixture.dev, "OUT 01");
  tls_init_logs = log_count (fixture.dev, "OUT 05");
  g_assert_cmpuint (init_logs, ==, 1);
  g_assert_cmpuint (tls_init_logs, ==, 1);
  g_assert_cmpuint (fixture.dev->init_count, ==, 1);

  for (guint round = 0; round < 5; round++)
    {
      /* Every round carries its own plane: a stale image cannot pass. */
      for (gsize i = 0; i < sizeof fixture.dev->plane; i++)
        fixture.dev->plane[i] = (guint8)(i * 17 + round * 53 + 3);

      fixture_capture (&fixture);

      g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);
      g_assert_cmpuint (fixture.capture_calls, ==, round + 1);
      g_assert_cmpuint (fixture.touch_calls, ==, round + 1);
      g_assert_cmpuint (fixture.arm_calls, ==, round + 1);
      assert_image (&fixture);
      g_assert_false (fixture.dev->get_img_without_touch);

      /* The channel and the session are reused: §6/§8 never run again. */
      g_assert_cmpuint (log_count (fixture.dev, "OUT 01"), ==, 1);
      g_assert_cmpuint (log_count (fixture.dev, "OUT 05"), ==, 1);
      g_assert_cmpuint (fixture.dev->init_count, ==, 1);
      g_assert_cmpuint (fixture.dev->commit_count, ==, 1);
    }

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_OK);
  g_assert_cmpuint (fixture.close_calls, ==, 1);
  /* The cleanup of the whole open happens once. */
  g_assert_cmpuint (log_count (fixture.dev, "OUT 02 v=0001 len=4 p=122f1117"), ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "OUT 03"), ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "OUT 12"), ==, 1);

  fixture_teardown (&fixture);
}

static void
test_multi_capture_third_round_fails (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  for (guint round = 0; round < 2; round++)
    {
      for (gsize i = 0; i < sizeof fixture.dev->plane; i++)
        fixture.dev->plane[i] = (guint8)(i * 11 + round * 41 + 5);

      fixture_capture (&fixture);

      g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);
      g_assert_cmpuint (fixture.capture_calls, ==, round + 1);
      assert_image (&fixture);
    }

  /* The third round sees no touch and must not deliver or reuse an image. */
  fixture.dev->send_finger_down = FALSE;
  fixture.image_len = 0;

  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_NO_TOUCH);
  g_assert_cmpuint (fixture.capture_calls, ==, 3);
  g_assert_cmpuint (fixture.image_len, ==, 0);
  /* Only the two completed rounds ever sent GET_IMG. */
  g_assert_cmpuint (log_count (fixture.dev, "OUT 09"), ==, 2);
  g_assert_false (fixture.dev->get_img_without_touch);
  /* A failure is not a reason to reinitialise. */
  g_assert_cmpuint (log_count (fixture.dev, "OUT 01"), ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "OUT 05"), ==, 1);

  /* One close for the open, and it still runs the reviewed cleanup once.  The
   * close report keeps the round's first cause, so its status is NO_TOUCH. */
  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_NO_TOUCH);
  g_assert_cmpuint (fixture.close_calls, ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "OUT 02 v=0001 len=4 p=122f1117"), ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "OUT 03"), ==, 1);
  g_assert_cmpuint (log_count (fixture.dev, "OUT 12"), ==, 1);
  g_assert_cmpstr (fixture_find_cleanup (&fixture, CMD_ARM)->cleanup_result, ==, "ok");
  g_assert_cmpstr (fixture_find_cleanup (&fixture, CMD_ABORT)->cleanup_result, ==, "ok");
  g_assert_cmpstr (fixture_find_cleanup (&fixture, CMD_SESSION_OFF)->cleanup_result, ==, "ok");

  fixture_teardown (&fixture);
}

static void
test_arm_short_completion (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->arm_actual_length = 3;
  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_cmpuint (fixture.arm_calls, ==, 0);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  /* The short ARM was never accepted, so no ARM_STOP is invented. */
  g_assert_false (log_contains (fixture.dev, "OUT 02 v=0001 len=4 p=122f1117"));

  fixture_teardown (&fixture);
}

static void
test_arm_io_completion (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->arm_io_failure = TRUE;
  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_cmpuint (fixture.arm_calls, ==, 0);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  /* An unsent/failed ARM is not followed by a conservative ARM_STOP. */
  g_assert_false (log_contains (fixture.dev, "OUT 02 v=0001 len=4 p=122f1117"));
  assert_log_tail (fixture.dev, (const gchar *const[]){
                                    "OUT 03 v=0000 len=0 p=",
                                    "OUT 12 v=0000 len=0 p=",
                                    NULL,
                                });

  fixture_teardown (&fixture);
}

static void
test_capture_requires_touch (void)
{
  Fixture fixture;
  static const gchar *const cleanup[] = {
    "OUT 02 v=0001 len=4 p=122f1117",
    "OUT 03 v=0000 len=0 p=",
    "OUT 12 v=0000 len=0 p=",
    NULL,
  };

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_finger_down = FALSE;
  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_NO_TOUCH);
  /* GET_IMG must never be sent without the touch event (SPEC §16.1). */
  g_assert_false (log_contains (fixture.dev, "OUT 09"));
  g_assert_cmpuint (fixture.touch_calls, ==, 0);
  g_assert_cmpuint (fixture.dev->timer.last_interval_ms, ==, FPC_DISUM_CAPTURE_WINDOW_MS);

  fixture_close (&fixture);
  assert_log_tail (fixture.dev, cleanup);

  fixture_teardown (&fixture);
}

static void
test_capture_ignores_irrelevant_events (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* Every reviewed code that is not the touch, the TLS data or the arm result is
   * recorded and ignored (SPEC §16.1). */
  fake_queue_event (fixture.dev->arm_events, EVT_HELLO, 4);
  fake_queue_event (fixture.dev->arm_events, EVT_ARM_RESULT, 0);
  fake_queue_event (fixture.dev->arm_events, EVT_DEAD_PIXEL_REPORT, 0);
  fake_queue_event (fixture.dev->arm_events, EVT_FINGER_UP, 0);
  fake_queue_event (fixture.dev->arm_events, EVT_USB_LOGS, 0);
  fake_queue_event (fixture.dev->arm_events, EVT_TLS_KEY, 0);
  fake_queue_event (fixture.dev->arm_events, EVT_REFRESH_SENSOR, 0);
  fake_queue_event (fixture.dev->img_events, EVT_HELLO, 4);

  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);
  assert_image (&fixture);
  g_assert_true (fixture.dev->finger_down_seen);

  /* Seven events before the touch and one during the image transfer are recorded
   * and ignored; the touch and the TLS frames are consumed, so they are not
   * recorded (SPEC §16.1). */
  {
    static const guint32 ignored[] = {
      EVT_HELLO,
      EVT_ARM_RESULT,
      EVT_DEAD_PIXEL_REPORT,
      EVT_FINGER_UP,
      EVT_USB_LOGS,
      EVT_TLS_KEY,
      EVT_REFRESH_SENSOR,
      EVT_HELLO,
    };

    g_assert_cmpuint (fpc_disum_session_event_count (fixture.session), ==, G_N_ELEMENTS (ignored));
    for (guint i = 0; i < G_N_ELEMENTS (ignored); i++)
      g_assert_cmpuint (fpc_disum_session_event (fixture.session, i)->code, ==, ignored[i]);

    g_assert_cmpuint (fpc_disum_session_event (fixture.session, 0)->third_field, ==, 4);
    g_assert_cmpint (find_event (fixture.session, EVT_FINGER_DWN), ==, -1);
    g_assert_cmpint (find_event (fixture.session, EVT_TLS), ==, -1);
  }

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_rejects_unreviewed_event (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fake_queue_event (fixture.dev->arm_events, EVT_UNREVIEWED, 0xaabbccdd);

  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT);
  /* The frame is recorded verbatim, third field included, and never treated as the
   * touch (SPEC §16.1). */
  g_assert_cmpuint (fpc_disum_session_event_count (fixture.session), ==, 1);
  g_assert_cmpuint (fpc_disum_session_event (fixture.session, 0)->code, ==, EVT_UNREVIEWED);
  g_assert_cmpuint (fpc_disum_session_event (fixture.session, 0)->third_field, ==, 0xaabbccdd);
  g_assert_cmpuint (
      fpc_disum_session_event (fixture.session, 0)->total_len, ==, FPC_DISUM_FRAME_HEADER_LEN);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_rejects_reserved_arm_result (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* The reserved arm-result value fails the transaction without being named
   * (SPEC §16.1). */
  fake_queue_event (fixture.dev->arm_events, EVT_ARM_RESULT, RESERVED_ARM_RESULT);

  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_ARM_RESULT);
  g_assert_cmpuint (fpc_disum_session_event_count (fixture.session), ==, 1);
  g_assert_cmpuint (
      fpc_disum_session_event (fixture.session, 0)->third_field, ==, RESERVED_ARM_RESULT);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_frame_segmentation (void)
{
  static const struct
  {
    guint tls_frame_size;
    guint read_chunk;
  } variants[] = {
    { 0, 0 },     /* one EVT_TLS frame per flight, full packets */
    { 0, 7 },     /* frames cut across bulk packets */
    { 1009, 64 }, /* the reviewed reference segmentation */
    { 1, 1 },     /* one TLS byte per frame and per packet */
    { 100, 1 },   /* frames and packets both arbitrary */
  };

  for (guint v = 0; v < G_N_ELEMENTS (variants); v++)
    {
      Fixture fixture;

      fixture_setup (&fixture);
      fixture_init_ok (&fixture);

      fixture.dev->tls_frame_size = variants[v].tls_frame_size;
      fixture.dev->read_chunk = variants[v].read_chunk;

      fixture_capture (&fixture);

      g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);
      assert_image (&fixture);
      /* The number of EVT_TLS frames is never assumed (SPEC §5/§16.4). */
      g_assert_cmpuint (fixture.dev->tls_frames, >, 0);
      if (variants[v].tls_frame_size == 1)
        g_assert_cmpuint (fixture.dev->tls_frames, >, 100);

      fixture_close (&fixture);
      fixture_teardown (&fixture);
    }
}

static void
test_capture_partial_then_complete (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* One 12 B finger-down frame arrives as TIMEOUT+7 B, then OK+5 B.  The first
   * completion is not a broken frame and the touch is reported once. */
  fake_queue_read (fixture.dev, FPC_DISUM_TRANSPORT_TIMEOUT, 7);

  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);
  g_assert_cmpuint (fixture.touch_calls, ==, 1);
  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_false (fixture.dev->get_img_without_touch);
  assert_image (&fixture);

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_zlp_and_zero_timeout (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* 7 B, then a ZLP, then TIMEOUT+0, then the remaining 5 B: the partial frame is
   * never cleared and exactly one frame is delivered. */
  fake_queue_read (fixture.dev, FPC_DISUM_TRANSPORT_TIMEOUT, 7);
  fake_queue_read (fixture.dev, FPC_DISUM_TRANSPORT_OK, 0);
  fake_queue_read (fixture.dev, FPC_DISUM_TRANSPORT_TIMEOUT, 0);

  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);
  g_assert_cmpuint (fixture.touch_calls, ==, 1);
  g_assert_false (fixture.dev->get_img_without_touch);
  assert_image (&fixture);

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_bad_frame_length (void)
{
  static const guint32 lengths[] = { 11, 65537 };

  for (guint i = 0; i < G_N_ELEMENTS (lengths); i++)
    {
      Fixture fixture;

      fixture_setup (&fixture);
      fixture_init_ok (&fixture);

      FpcDisumDiagnosticRecord *terminal;

      fixture.dev->app_total_len = lengths[i];
      /* Keep every EVT_TLS frame inside the reviewed frame bound even for the
       * deliberately oversized application length. */
      fixture.dev->tls_frame_size = 1024;

      fixture_capture (&fixture);

      g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_PROTOCOL);
      g_assert_cmpuint (fixture.capture_calls, ==, 1);
      g_assert_cmpuint (fpc_disum_session_event_count (fixture.session), ==, 0);

      /* The unconsumed application partial is retained, not resynchronised. */
      terminal = fixture_last_round (&fixture);
      g_assert_nonnull (terminal);
      g_assert_cmpstr (terminal->primary_result, ==, "protocol");
      g_assert_true (terminal->discarded_valid);
      g_assert_cmpuint (terminal->discarded_length, >=, 8);

      fixture_close (&fixture);
      fixture_teardown (&fixture);
    }
}

static void
test_capture_without_image (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_image = FALSE;
  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_NO_IMAGE);
  g_assert_true (log_contains (fixture.dev, "OUT 09"));
  g_assert_cmpuint (fixture.dev->timer.last_interval_ms, ==, FPC_DISUM_CAPTURE_WINDOW_MS);

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_io_error (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* A failed transfer is an I/O failure, never "no data" (SPEC §16.2). */
  fixture.dev->fail_bulk_now = TRUE;
  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_cmpuint (fixture.capture_calls, ==, 1);

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_integrity_unknown (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* An unknown completion length must fail, never become an empty image read. */
  fixture.dev->read_integrity_next = TRUE;
  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_cmpuint (fixture.image_len, ==, 0);

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_capture_rejects_bad_application_message (void)
{
  Fixture fixture;

  /* A message that cannot be the reviewed image: wrong length, then wrong code. */
  fixture_setup (&fixture);
  fixture_init_ok (&fixture);
  fixture.dev->app_total_len = 100;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_PROTOCOL);
  fixture_close (&fixture);
  fixture_teardown (&fixture);

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);
  fixture.dev->app_code = 9;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_PROTOCOL);
  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_application_before_get_img (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* Decryptable application data arrives before any GET_IMG: the gate must fail
   * with the fixed message instead of delivering an image (SPEC §4.5). */
  fixture.dev->send_finger_down = FALSE;
  fixture.dev->send_app_before_touch = TRUE;
  fixture_capture (&fixture);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_PROTOCOL);
  g_assert_cmpstr (fixture.capture_message, ==, "application data arrived before GET_IMG");
  g_assert_cmpuint (fixture.image_len, ==, 0);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_tls_wait_fatal_alert (void)
{
  Fixture fixture;
  /* TLS 1.2 fatal alert: handshake_failure. */
  static const guint8 alert[] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28 };

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_finger_down = FALSE;
  fixture.dev->auto_fire_idle = FALSE;
  fixture_capture (&fixture);
  g_assert_cmpuint (fixture.capture_calls, ==, 0);
  g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_READ);

  fake_push_frame (fixture.dev, EVT_TLS, 0, alert, sizeof alert);
  fake_complete_held_from_out (fixture.dev, FPC_DISUM_TRANSPORT_OK);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_TLS);
  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_tls_wait_close_notify (void)
{
  Fixture fixture;
  /* TLS 1.2 close_notify alert. */
  static const guint8 close_notify[] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x01, 0x00 };

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_finger_down = FALSE;
  fixture.dev->auto_fire_idle = FALSE;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_READ);

  fake_push_frame (fixture.dev, EVT_TLS, 0, close_notify, sizeof close_notify);
  fake_complete_held_from_out (fixture.dev, FPC_DISUM_TRANSPORT_OK);

  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_TLS);
  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_cancel_from_touch_handler (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* Cancelling from the touch callback must stop the transaction before GET_IMG
   * is put on the wire. */
  fixture.cancel_on_touch = TRUE;
  fixture_capture (&fixture);

  g_assert_cmpuint (fixture.touch_calls, ==, 1);
  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);
  g_assert_false (log_contains (fixture.dev, "OUT 09"));

  fixture_close (&fixture);
  g_assert_cmpuint (fixture.close_calls, ==, 1);
  /* ARM was accepted and delivered, so cleanup is conservative. */
  g_assert_true (log_contains (fixture.dev, "OUT 02 v=0001 len=4 p=122f1117"));

  fixture_teardown (&fixture);
}

static void
test_cancel_during_touch_wait (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_finger_down = FALSE; /* nothing arrives */
  fixture.dev->auto_fire_idle = FALSE;   /* the read stays in flight */
  fixture_capture (&fixture);

  g_assert_cmpuint (fixture.capture_calls, ==, 0);
  g_assert_false (fpc_disum_session_is_cancelled (fixture.session));

  fpc_disum_session_cancel (fixture.session);

  g_assert_true (fpc_disum_session_is_cancelled (fixture.session));
  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);

  fixture_close (&fixture);
  {
    static const gchar *const cleanup[] = {
      "OUT 02 v=0001 len=4 p=122f1117",
      "OUT 03 v=0000 len=0 p=",
      "OUT 12 v=0000 len=0 p=",
      NULL,
    };

    assert_log_tail (fixture.dev, cleanup);
  }
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);

  fixture_teardown (&fixture);
}

static void
test_cancel_during_init (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->respond_handshake = FALSE;
  fixture.dev->auto_fire_idle = FALSE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);
  g_assert_cmpuint (fixture.init_calls, ==, 0);

  fpc_disum_session_cancel (fixture.session);

  g_assert_cmpuint (fixture.init_calls, ==, 1);
  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);

  fixture_close (&fixture);
  /* The session was never armed, so no ARM_STOP is sent (SPEC §16.3). */
  g_assert_false (log_contains (fixture.dev, "OUT 02"));
  {
    static const gchar *const cleanup[] = {
      "OUT 03 v=0000 len=0 p=",
      "OUT 12 v=0000 len=0 p=",
      NULL,
    };

    assert_log_tail (fixture.dev, cleanup);
  }

  fixture_teardown (&fixture);
}

static void
test_cancel_tail_is_discarded (void)
{
  Fixture fixture;
  FpcDisumDiagnosticRecord *terminal;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* The read is held; cancelled operations are completed by the transport later. */
  fixture.dev->send_finger_down = FALSE;
  fixture.dev->auto_fire_idle = FALSE;
  fixture.dev->defer_cancel = TRUE;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_READ);

  fpc_disum_session_cancel (fixture.session);

  /* The cancel request itself is not a completion: nothing is reported yet. */
  g_assert_true (fixture.dev->cancel_deferred);
  g_assert_cmpuint (fixture.capture_calls, ==, 0);
  g_assert_false (log_contains (fixture.dev, "OUT 03"));

  /* The cancelled read still delivers a complete image stream.  None of it may
   * become an image; the bytes are accounted as discarded. */
  fake_send_image (fixture.dev);
  fake_complete_held_from_out (fixture.dev, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);

  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);
  g_assert_cmpuint (fixture.image_len, ==, 0);

  terminal = fixture_last_round (&fixture);
  g_assert_nonnull (terminal);
  g_assert_cmpstr (terminal->primary_result, ==, "cancelled");
  g_assert_true (terminal->discarded_valid);
  g_assert_cmpuint (terminal->discarded_length, >, 0);

  fixture_close (&fixture);
  g_assert_cmpuint (fixture.close_calls, ==, 1);
  fixture_teardown (&fixture);
}

static void
test_deadline_preserves_first_cause (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_finger_down = FALSE;
  fixture.dev->auto_fire_idle = FALSE;
  fixture.dev->defer_cancel = TRUE;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_READ);

  /* The window expires while the read is still in flight. */
  g_assert_true (fake_timer_fire (&fixture.dev->timer));
  g_assert_cmpuint (fixture.capture_calls, ==, 0);

  /* The cancel completion arrives much later and must not change the cause. */
  fake_complete_held_data (fixture.dev, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, NULL, 0);

  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_NO_TOUCH);

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_NO_TOUCH);
  fixture_teardown (&fixture);
}

static void
test_deadline_handshake (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture.dev->respond_handshake = FALSE;
  fixture.dev->auto_fire_idle = FALSE;
  fixture.dev->defer_cancel = TRUE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);
  g_assert_cmpuint (fixture.init_calls, ==, 0);

  g_assert_true (fake_timer_fire (&fixture.dev->timer));
  g_assert_cmpuint (fixture.init_calls, ==, 0);

  fake_complete_held_data (fixture.dev, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, NULL, 0);

  g_assert_cmpuint (fixture.init_calls, ==, 1);
  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_TLS);

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

static void
test_handshake_deadline_is_absolute (void)
{
  Fixture fixture;
  guint adds_after_init;

  fixture_setup (&fixture);
  /* Hold the first TLS_DATA fragment so the handshake is suspended mid-flight. */
  fixture.dev->hold_request = CMD_TLS_DATA;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

  g_assert_cmpuint (fixture.init_calls, ==, 0);
  g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_OUT);
  /* One add for the reopen window and one for the handshake transaction. */
  g_assert_cmpuint (fixture.dev->timer.adds, ==, 2);
  g_assert_cmpuint (fixture.dev->timer.last_interval_ms, ==, FPC_DISUM_HANDSHAKE_TIMEOUT_MS);

  adds_after_init = fixture.dev->timer.adds;
  fake_complete_held_control (
      fixture.dev, FPC_DISUM_TRANSPORT_OK, fixture.dev->held_requested_length);

  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_OK);
  /* TLS_DATA progress never extends the absolute handshake deadline. */
  g_assert_cmpuint (fixture.dev->timer.adds, ==, adds_after_init);

  fixture_teardown (&fixture);
}

static void
test_handshake_output_deadline (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  /* Hold the first TLS_DATA fragment; the deadline expires while it is pending. */
  fixture.dev->hold_request = CMD_TLS_DATA;
  fixture.dev->defer_cancel = TRUE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);
  g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_OUT);

  g_assert_true (fake_timer_fire (&fixture.dev->timer));
  g_assert_cmpuint (fixture.init_calls, ==, 0);
  g_assert_false (log_contains (fixture.dev, "OUT 02"));

  fake_complete_held_control (
      fixture.dev, FPC_DISUM_TRANSPORT_OK, fixture.dev->held_requested_length);

  g_assert_cmpuint (fixture.init_calls, ==, 1);
  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_TLS);
  g_assert_false (log_contains (fixture.dev, "OUT 02"));

  fixture_teardown (&fixture);
}

/* ---------------------------------------------------- lifecycle test cases */

/*
 * Every operation the initialisation can be waiting on, taken one at a time:
 * cancel there, require exactly one cancellation report and then a complete
 * cleanup.  Indices: 1 = 0x08, 2 = GET_STATE, 3 = CMD_INIT, 4 = the handle swap,
 * 5 = GET_TLS_KEY, 6 = TLS_INIT.
 */
static void
test_cancel_at_every_init_suspension_point (void)
{
  static const gchar *const cleanup[] = {
    "OUT 03 v=0000 len=0 p=",
    "OUT 12 v=0000 len=0 p=",
    NULL,
  };

  for (guint op = 1; op <= 6; op++)
    {
      Fixture fixture;

      fixture_setup (&fixture);
      fixture.dev->hold_at = op;

      fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);

      g_assert_cmpuint (fixture.init_calls, ==, 0);
      g_assert_cmpint (fixture.dev->held_kind, !=, HOLD_NONE);

      fpc_disum_session_cancel (fixture.session);

      g_assert_cmpuint (fixture.init_calls, ==, 1);
      g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);
      g_assert_true (fpc_disum_session_is_cancelled (fixture.session));

      /* Cancelling twice must not report a second time. */
      fpc_disum_session_cancel (fixture.session);
      g_assert_cmpuint (fixture.init_calls, ==, 1);

      fixture_close (&fixture);
      g_assert_cmpuint (fixture.close_calls, ==, 1);
      g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);
      /* The session was never armed, so no ARM_STOP is sent (SPEC §16.3). */
      g_assert_false (log_contains (fixture.dev, "OUT 02"));
      assert_log_tail (fixture.dev, cleanup);

      fixture_teardown (&fixture);
    }
}

/* Cancelling while ARM or GET_IMG is in flight: the cleanup depends on whether
 * ARM may have been accepted (SPEC §16.3). */
static void
test_cancel_at_capture_suspension_points (void)
{
  static const guint8 requests[] = { CMD_ARM, CMD_GET_IMG };
  static const gchar *const cleanup_armed[] = {
    "OUT 02 v=0001 len=4 p=122f1117",
    "OUT 03 v=0000 len=0 p=",
    "OUT 12 v=0000 len=0 p=",
    NULL,
  };

  for (guint i = 0; i < G_N_ELEMENTS (requests); i++)
    {
      Fixture fixture;

      fixture_setup (&fixture);
      fixture_init_ok (&fixture);

      fixture.dev->hold_request = requests[i];
      fixture_capture (&fixture);

      g_assert_cmpuint (fixture.capture_calls, ==, 0);
      g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_OUT);

      fpc_disum_session_cancel (fixture.session);

      g_assert_cmpuint (fixture.capture_calls, ==, 1);
      g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);
      /* A cancelled capture never delivers an image. */
      g_assert_cmpuint (fixture.image_len, ==, 0);

      fixture_close (&fixture);

      assert_log_tail (fixture.dev, cleanup_armed);

      fixture_teardown (&fixture);
    }
}

/* Cancelling while the image transfer is in flight. */
static void
test_cancel_during_image_transfer (void)
{
  Fixture fixture;
  static const gchar *const cleanup[] = {
    "OUT 02 v=0001 len=4 p=122f1117",
    "OUT 03 v=0000 len=0 p=",
    "OUT 12 v=0000 len=0 p=",
    NULL,
  };

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* The touch arrives and GET_IMG goes out, but the image never completes. */
  fixture.dev->send_image = FALSE;
  fixture.dev->auto_fire_idle = FALSE;
  fixture_capture (&fixture);

  g_assert_true (log_contains (fixture.dev, "OUT 09"));
  g_assert_cmpuint (fixture.capture_calls, ==, 0);

  fpc_disum_session_cancel (fixture.session);

  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_CANCELLED);

  fixture_close (&fixture);
  assert_log_tail (fixture.dev, cleanup);
  fixture_teardown (&fixture);
}

static void
second_init_done (FpcDisumSessionStatus status, const gchar *message, gpointer user_data)
{
  *(FpcDisumSessionStatus *)user_data = status;
}

/*
 * A failed session is closed with the same cleanup, keeps its root cause, and a
 * new session initialises normally afterwards (SPEC §16.3).
 */
static void
test_close_after_failure_then_reopen (void)
{
  Fixture fixture;
  FpcDisumSession *second;
  FpcDisumTimeoutSource source = { 0 };
  FpcDisumSessionStatus second_status = FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT;

  fixture_setup (&fixture);
  fixture.dev->envelope_corrupt = TRUE;

  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);
  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_ENVELOPE);

  fixture_close (&fixture);
  /* The cleanup ran and the report still carries the first root cause. */
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_ENVELOPE);

  fixture.dev->envelope_corrupt = FALSE;
  second = fpc_disum_session_new (&fixture.dev->transport);
  g_assert_nonnull (second);
  source.add = fake_timer_add;
  source.remove = fake_timer_remove;
  source.context = &fixture.dev->timer;
  fpc_disum_session_set_timeout_source (second, &source);

  fpc_disum_session_init_async (second, second_init_done, &second_status);
  g_assert_cmpint (second_status, ==, FPC_DISUM_SESSION_OK);

  fpc_disum_session_free (second);
  fixture_teardown (&fixture);
}

/* Closing twice is harmless and still reports once per call. */
static void
test_close_is_repeatable (void)
{
  Fixture fixture;
  static const gchar *const first_cleanup[] = {
    "OUT 02 v=0001 len=4 p=122f1117",
    "OUT 03 v=0000 len=0 p=",
    "OUT 12 v=0000 len=0 p=",
    NULL,
  };
  static const gchar *const second_cleanup[] = {
    "OUT 03 v=0000 len=0 p=",
    "OUT 12 v=0000 len=0 p=",
    NULL,
  };

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_OK);
  assert_log_tail (fixture.dev, first_cleanup);

  /* The ARM_STOP cleared the armed state, so the second cleanup only sends
   * ABORT and SESSION_OFF. */
  fixture_close (&fixture);
  g_assert_cmpuint (fixture.close_calls, ==, 2);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_OK);
  assert_log_tail (fixture.dev, second_cleanup);

  fixture_teardown (&fixture);
}

static void
test_close_keeps_first_root_cause (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_finger_down = FALSE;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_NO_TOUCH);

  /* A failing cleanup must not overwrite the first root cause, but every cleanup
   * command is still attempted (SPEC §16.3). */
  fixture.dev->fail_abort = TRUE;
  fixture_close (&fixture);

  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_NO_TOUCH);
  {
    static const gchar *const cleanup[] = {
      "OUT 02 v=0001 len=4 p=122f1117",
      "OUT 03 v=0000 len=0 p=",
      "OUT 12 v=0000 len=0 p=",
      NULL,
    };

    assert_log_tail (fixture.dev, cleanup);
  }

  /* Each cleanup command is recorded separately; the ABORT error does not hide
   * the first cause or the successful commands around it. */
  {
    FpcDisumDiagnosticRecord *arm = fixture_find_cleanup (&fixture, CMD_ARM);
    FpcDisumDiagnosticRecord *abort = fixture_find_cleanup (&fixture, CMD_ABORT);
    FpcDisumDiagnosticRecord *off = fixture_find_cleanup (&fixture, CMD_SESSION_OFF);
    FpcDisumDiagnosticRecord *terminal = fixture_last_round (&fixture);

    g_assert_nonnull (arm);
    g_assert_nonnull (abort);
    g_assert_nonnull (off);
    g_assert_cmpstr (arm->cleanup_result, ==, "ok");
    g_assert_cmpuint (arm->actual_length, ==, 4);
    g_assert_cmpstr (abort->cleanup_result, ==, "io");
    g_assert_cmpint (abort->status, ==, FPC_DISUM_TRANSPORT_ERROR_IO);
    g_assert_cmpstr (off->cleanup_result, ==, "ok");
    g_assert_cmpstr (terminal->primary_result, ==, "no-touch");
    g_assert_cmpstr (terminal->cleanup_result, ==, "io");
  }

  fixture_teardown (&fixture);
}

static void
test_close_reports_cleanup_failure (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture_capture (&fixture);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);

  fixture.dev->fail_abort = TRUE;
  fixture_close (&fixture);

  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_true (log_contains (fixture.dev, "OUT 03"));
  g_assert_true (log_contains (fixture.dev, "OUT 12"));

  fixture_teardown (&fixture);
}

static void
test_generation_change_after_tls (void)
{
  Fixture fixture;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->send_finger_down = FALSE;
  fixture.dev->auto_fire_idle = FALSE;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.dev->held_kind, ==, HOLD_READ);

  /* A real generation change after TLS started ends the open; it is not
   * re-handshaked transparently (SPEC §6). */
  fixture.dev->generation++;
  fake_complete_held_data (fixture.dev, FPC_DISUM_TRANSPORT_OK, NULL, 0);

  g_assert_cmpuint (fixture.capture_calls, ==, 1);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_CONNECTION_CHANGED);

  fixture_close (&fixture);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_CONNECTION_CHANGED);
  fixture_teardown (&fixture);
}

static void
test_abnormal_reset_is_not_recovered (void)
{
  Fixture fixture;
  FpcDisumSession *fresh_session;
  FakeDevice *fresh_dev;
  FpcDisumTimeoutSource source = { 0 };
  FpcDisumSessionStatus fresh_status = FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT;
  guint log_before;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  /* An abnormal NO_DEVICE during the first capture ends it as I/O and records
   * the abnormal reset in the session. */
  fixture.dev->read_no_device_next = TRUE;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_cmpuint (fixture.capture_calls, ==, 1);

  /* Attempting automatic recovery on the same damaged session is refused before
   * any USB request. */
  log_before = fixture.dev->log->len;
  fpc_disum_session_init_async (fixture.session, fixture_init_done, &fixture);
  g_assert_cmpuint (fixture.init_calls, ==, 2); /* the successful open, then this refusal */
  g_assert_cmpint (fixture.init_status, ==, FPC_DISUM_SESSION_ERROR_UNSUPPORTED_RESET_RECOVERY);
  g_assert_cmpuint (fixture.dev->log->len, ==, log_before);

  fpc_disum_session_capture_async (fixture.session, fixture_capture_done, &fixture);
  g_assert_cmpuint (fixture.capture_calls, ==, 2);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_ERROR_UNSUPPORTED_RESET_RECOVERY);
  g_assert_cmpuint (fixture.dev->log->len, ==, log_before);

  /* Closing still runs, but the dead handle records unavailable cleanup instead
   * of pretending to have sent commands. */
  fixture_close (&fixture);
  g_assert_cmpuint (fixture.close_calls, ==, 1);
  g_assert_cmpint (fixture.close_status, ==, FPC_DISUM_SESSION_ERROR_IO);
  g_assert_cmpuint (fixture.dev->log->len, ==, log_before);
  g_assert_cmpstr (fixture_find_cleanup (&fixture, CMD_ABORT)->cleanup_result, ==, "unavailable");

  fpc_disum_session_free (fixture.session);
  fake_device_free (fixture.dev);
  g_ptr_array_free (fixture.records, TRUE);
  fpc_disum_diagnostics_unref (fixture.diagnostics);

  /* A brand-new session on a healthy transport is not poisoned. */
  fixture.dev = fake_device_new_standalone ();
  fixture.records = g_ptr_array_new_with_free_func (g_free);
  fixture.diagnostics = fpc_disum_diagnostics_new ();
  fixture.dev->diagnostics = fixture.diagnostics;
  fpc_disum_diagnostics_set_observer (fixture.diagnostics, fixture_diagnostic, &fixture);
  fresh_dev = fixture.dev;
  fresh_session = fpc_disum_session_new (&fresh_dev->transport);
  source.add = fake_timer_add;
  source.remove = fake_timer_remove;
  source.context = &fresh_dev->timer;
  fpc_disum_session_set_timeout_source (fresh_session, &source);
  fpc_disum_session_init_async (fresh_session, second_init_done, &fresh_status);
  g_assert_cmpint (fresh_status, ==, FPC_DISUM_SESSION_OK);
  fpc_disum_session_free (fresh_session);
  fake_device_free (fresh_dev);
  g_ptr_array_free (fixture.records, TRUE);
  fpc_disum_diagnostics_unref (fixture.diagnostics);
}

static FakeDevice *
fake_device_new_standalone (void)
{
  return fake_device_new (accepted_envelope ());
}

static void
test_completion_records_are_complete (void)
{
  Fixture fixture;
  GHashTable *submits;
  GHashTable *completes;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  guint n_submit;
  guint n_complete;
  guint64 last_op_id = 0;
  gint64 last_time = 0;

  fixture_setup (&fixture);
  fixture_init_ok (&fixture);

  fixture.dev->read_chunk = 1;
  fixture_capture (&fixture);
  g_assert_cmpint (fixture.capture_status, ==, FPC_DISUM_SESSION_OK);

  submits = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  completes = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);

  for (guint i = 0; i < fixture.records->len; i++)
    {
      FpcDisumDiagnosticRecord *record = g_ptr_array_index (fixture.records, i);
      guint64 *id;

      if (record->t_us != 0)
        {
          g_assert_cmpint (record->t_us, >=, last_time);
          last_time = record->t_us;
        }

      if (record->kind == FPC_DISUM_DIAGNOSTIC_KIND_SUBMIT)
        {
          id = g_new (guint64, 1);
          *id = record->op_id;
          g_hash_table_insert (submits, id, GINT_TO_POINTER (1));
          g_assert_cmpuint (record->op_id, >, last_op_id);
          last_op_id = record->op_id;
        }
      else if (record->kind == FPC_DISUM_DIAGNOSTIC_KIND_COMPLETE)
        {
          id = g_new (guint64, 1);
          *id = record->op_id;
          g_hash_table_insert (completes, id, GINT_TO_POINTER (1));
        }
    }

  n_submit = g_hash_table_size (submits);
  n_complete = g_hash_table_size (completes);

  /* The image is streamed one byte at a time, far beyond 256 completions. */
  g_assert_cmpuint (n_submit, >, 256);
  g_assert_cmpuint (n_complete, ==, n_submit);

  g_hash_table_iter_init (&iter, submits);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_assert_true (g_hash_table_contains (completes, key));

  g_hash_table_unref (submits);
  g_hash_table_unref (completes);

  fixture_close (&fixture);
  fixture_teardown (&fixture);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/session/init-command-order", test_init_command_order);
  g_test_add_func ("/session/init-handshake-timeout", test_init_handshake_timeout);
  g_test_add_func ("/session/init-envelope-rejected", test_init_envelope_rejected);
  g_test_add_func ("/session/init-reopen-failure", test_init_reopen_failure);
  g_test_add_func ("/session/init-commit-failure", test_init_commit_failure);
  g_test_add_func ("/session/init-short-control-out", test_init_short_control_out);
  g_test_add_func ("/session/init-get-tls-key-retry", test_init_get_tls_key_retry);
  g_test_add_func ("/session/init-tls-identity-rejected", test_init_tls_identity_rejected);
  g_test_add_func ("/session/capture-command-order", test_capture_command_order);
  g_test_add_func ("/session/multi-capture-five-rounds", test_multi_capture_five_rounds);
  g_test_add_func (
      "/session/multi-capture-third-round-fails", test_multi_capture_third_round_fails);
  g_test_add_func ("/session/arm-short-completion", test_arm_short_completion);
  g_test_add_func ("/session/arm-io-completion", test_arm_io_completion);
  g_test_add_func ("/session/capture-requires-touch", test_capture_requires_touch);
  g_test_add_func (
      "/session/capture-ignores-irrelevant-events", test_capture_ignores_irrelevant_events);
  g_test_add_func (
      "/session/capture-rejects-unreviewed-event", test_capture_rejects_unreviewed_event);
  g_test_add_func (
      "/session/capture-rejects-reserved-arm-result", test_capture_rejects_reserved_arm_result);
  g_test_add_func ("/session/capture-frame-segmentation", test_capture_frame_segmentation);
  g_test_add_func ("/session/capture-partial-then-complete", test_capture_partial_then_complete);
  g_test_add_func ("/session/capture-zlp-and-zero-timeout", test_capture_zlp_and_zero_timeout);
  g_test_add_func ("/session/capture-bad-frame-length", test_capture_bad_frame_length);
  g_test_add_func ("/session/capture-without-image", test_capture_without_image);
  g_test_add_func ("/session/capture-io-error", test_capture_io_error);
  g_test_add_func ("/session/capture-integrity-unknown", test_capture_integrity_unknown);
  g_test_add_func ("/session/capture-rejects-bad-application-message",
      test_capture_rejects_bad_application_message);
  g_test_add_func ("/session/application-before-get-img", test_application_before_get_img);
  g_test_add_func ("/session/tls-wait-fatal-alert", test_tls_wait_fatal_alert);
  g_test_add_func ("/session/tls-wait-close-notify", test_tls_wait_close_notify);
  g_test_add_func ("/session/cancel-from-touch-handler", test_cancel_from_touch_handler);
  g_test_add_func ("/session/cancel-during-touch-wait", test_cancel_during_touch_wait);
  g_test_add_func ("/session/cancel-during-init", test_cancel_during_init);
  g_test_add_func ("/session/cancel-tail-is-discarded", test_cancel_tail_is_discarded);
  g_test_add_func ("/session/deadline-preserves-first-cause", test_deadline_preserves_first_cause);
  g_test_add_func ("/session/deadline-handshake", test_deadline_handshake);
  g_test_add_func ("/session/handshake-deadline-is-absolute", test_handshake_deadline_is_absolute);
  g_test_add_func ("/session/handshake-output-deadline", test_handshake_output_deadline);
  g_test_add_func (
      "/session/cancel-at-every-init-suspension-point", test_cancel_at_every_init_suspension_point);
  g_test_add_func (
      "/session/cancel-at-capture-suspension-points", test_cancel_at_capture_suspension_points);
  g_test_add_func ("/session/cancel-during-image-transfer", test_cancel_during_image_transfer);
  g_test_add_func (
      "/session/close-after-failure-then-reopen", test_close_after_failure_then_reopen);
  g_test_add_func ("/session/close-is-repeatable", test_close_is_repeatable);
  g_test_add_func ("/session/close-keeps-first-root-cause", test_close_keeps_first_root_cause);
  g_test_add_func ("/session/close-reports-cleanup-failure", test_close_reports_cleanup_failure);
  g_test_add_func ("/session/generation-change-after-tls", test_generation_change_after_tls);
  g_test_add_func (
      "/session/abnormal-reset-is-not-recovered", test_abnormal_reset_is_not_recovered);
  g_test_add_func (
      "/session/completion-records-are-complete", test_completion_records_are_complete);

  return g_test_run ();
}
