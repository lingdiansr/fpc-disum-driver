/*
 * FPC Disum (10a5:9201) libusb transport.
 *
 * Reference: docs/CLEANROOM_SPEC.md §3.1 (control channel), §3.2/§4.4 (bulk IN
 * frames and cross-packet aggregation), §6 step 4 (handle swap after CMD_INIT),
 * §16.2 (USB error handling).
 *
 * The public GUsb API drops the transferred length on failure, and the reviewed
 * handle swap in §6 needs a handle the libfprint core does not own.  This file
 * therefore uses a private libusb-1.0 context and a raw handle: the core's GUsb
 * object stays owned (and opened) by the core, while every wire transfer is
 * issued here with libusb's asynchronous API.  Completion status and the real
 * transferred length are taken from the libusb transfer, never inferred.
 *
 * libusb is driven from a persistent GSource attached to the thread-default main
 * context; poll descriptors come from libusb_get_pollfds() and are added or
 * removed through libusb_set_pollfd_notifiers().  There is no polling loop and no
 * extra thread.  Transfer callbacks only record the outcome; the caller is
 * notified after libusb_handle_events_timeout_completed() has returned.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_usb.h"

#include <poll.h>
#include <string.h>

#include <gio/gio.h>
#include <gusb.h>
#include <libusb.h>

#include "fpi-device.h"
#include "fpi-log.h"
#include "fpc_disum_diagnostics.h"
#include "fpc_disum_tls.h"

/* SPEC §1/§3.2: the only data endpoint is 0x82 IN BULK, maxPacketSize 64. */
#define FPC_DISUM_USB_BULK_IN_EP 0x82
#define FPC_DISUM_USB_BULK_READ_SIZE 64
#define FPC_DISUM_USB_BULK_TIMEOUT_MS 200

/* SPEC §1: a single interface, num 0, alt 0. */
#define FPC_DISUM_USB_INTERFACE 0
/* SPEC §16.2: the device has a single configuration, which is already active. */
#define FPC_DISUM_USB_CONFIGURATION 1

/* SPEC §3.1: a command is a short vendor control transfer; a bounded timeout
 * keeps a stalled device from wedging the session. */
#define FPC_DISUM_USB_CONTROL_TIMEOUT_MS 5000

/* SPEC §6 step 4: a fresh list is taken 0.2 s apart within the caller's deadline. */
#define FPC_DISUM_USB_REOPEN_RETRY_MS 200

/* SPEC §6 step 4: a control GET_DESCRIPTOR of the device descriptor probes a
 * candidate handle for liveness. */
#define FPC_DISUM_USB_REQ_GET_DESCRIPTOR 0x06
#define FPC_DISUM_USB_DESC_DEVICE 0x0100
#define FPC_DISUM_USB_PROBE_BYTES 18

/* SPEC §4.1: the largest control IN request is the key request (1000 B). */
#define FPC_DISUM_USB_CONTROL_IN_MAX 1000

/* SPEC §4.1/§8: the widest control OUT data stage is a TLS fragment. */
#define FPC_DISUM_USB_CONTROL_OUT_MAX FPC_DISUM_TLS_DATA_MAX

/* A complete root-to-leaf port chain never exceeds this (USB 2.0/3.x). */
#define FPC_DISUM_USB_MAX_PORTS 8

/* Vendor control request types (wIndex is always 0). */
#define FPC_DISUM_USB_BM_REQUEST_OUT 0x40
#define FPC_DISUM_USB_BM_REQUEST_IN 0xC0
/* Standard GET_DESCRIPTOR: bmRequestType 0x80, request 0x06, value 0x0100. */
#define FPC_DISUM_USB_BM_STANDARD_IN 0x80

typedef struct _FpcDisumUsbOp FpcDisumUsbOp;
typedef struct _FpcDisumUsbPrivate FpcDisumUsbPrivate;

typedef enum
{
  FPC_DISUM_USB_OP_CONTROL_OUT,
  FPC_DISUM_USB_OP_CONTROL_IN,
  FPC_DISUM_USB_OP_BULK_READ,
  FPC_DISUM_USB_OP_REOPEN,
} FpcDisumUsbOpKind;

typedef enum
{
  /* A GET_DESCRIPTOR probe is running on the handle currently bound. */
  FPC_DISUM_USB_REOPEN_PROBE_OLD,
  /* No usable handle: waiting for the full identity to reappear. */
  FPC_DISUM_USB_REOPEN_WAIT,
  /* A candidate has been opened and claimed; its probe is running. */
  FPC_DISUM_USB_REOPEN_PROBE_NEW,
} FpcDisumUsbReopenStage;

/* The physical identity the raw handle is bound to, plus address (observation
 * only).  The complete port chain is kept; it is never truncated. */
typedef struct
{
  gboolean valid;
  guint8 bus;
  guint8 port_count;
  guint8 ports[FPC_DISUM_USB_MAX_PORTS];
  guint16 vid;
  guint16 pid;
  guint16 bcd_device;
  guint8 address;
} FpcDisumUsbBound;

/* One operation; at most one is in flight at a time.  Transfer buffers live in
 * the operation and are released only after its callback has returned. */
struct _FpcDisumUsbOp
{
  FpcDisumUsbPrivate *priv;     /* owned reference */
  FpcDisumTransport *transport; /* borrowed; never dereferenced after free */
  FpcDisumUsbOpKind kind;
  guint64 op_id;

  gboolean is_control;
  gboolean direction_in;
  guint8 bm_request_type;
  guint8 request;
  guint16 value;
  guint16 index;
  gsize requested_length;

  guint8 *buffer; /* owned; a control transfer starts with its setup packet */
  gsize buffer_len;
  struct libusb_transfer *transfer; /* owned */

  gboolean submitted;
  gboolean completed;
  gint submit_error; /* -LIBUSB_ERROR_* when submitted == FALSE */
  gint native_status;
  gint actual_length; /* saved by the libusb callback */
  gint64 submitted_us;
  gint64 completed_us;

  gboolean cancel_requested;
  gboolean slot_error;

  FpcDisumTransportDoneFunc done;
  FpcDisumTransportDataFunc data_done;
  gpointer user_data;

  /* Reopen state. */
  gint64 deadline_us;
  FpcDisumUsbReopenStage stage;
  GSource *retry_source;
  libusb_device_handle *probe_handle;
  libusb_device *probe_device;
};

struct _FpcDisumUsbPrivate
{
  gatomicrefcount ref_count;

  FpDevice *device;                 /* borrowed for the lifetime of the transport */
  FpcDisumDiagnostics *diagnostics; /* owned reference */

  libusb_context *context;
  libusb_device_handle *handle; /* claimed interface 0 */
  libusb_device *raw_device;    /* owned reference */

  GMainContext *main_context; /* owned reference */
  GSource *source;
  gboolean notifiers_installed;

  GQueue completions; /* FpcDisumUsbOp*, completed and awaiting delivery */
  FpcDisumUsbOp *in_flight;
  gboolean torn_down;

  FpiUsbIdentity identity; /* owned; the complete physical identity */
  FpcDisumUsbBound bound;
  guint64 handle_id;
  guint64 generation; /* 0: the core cannot prove a generation */

  GUsbDevice *core_candidate; /* owned; saved by reopen, checked by commit */
};

/* The public transport embeds the frozen vtable as its first member, mirroring
 * the offline device model in tests. */
typedef struct _FpcDisumUsbTransport
{
  FpcDisumTransport parent;
  FpcDisumUsbPrivate *priv;
} FpcDisumUsbTransport;

/* Custom GSource carrying libusb's poll descriptors.  The descriptor objects
 * are owned here because the main context only borrows them. */
typedef struct
{
  GSource source;
  FpcDisumUsbPrivate *priv;
  GPtrArray *pollfds; /* GPollFD */
  gint64 deadline_us;
} FpcDisumUsbSource;

static void priv_unref (FpcDisumUsbPrivate *priv);
static void op_free (FpcDisumUsbOp *op);
static void drain_completions (FpcDisumUsbPrivate *priv);
static void reopen_attempt (FpcDisumUsbOp *op);
static void reopen_transfer_completed (FpcDisumUsbOp *op);

static FpcDisumUsbPrivate *
priv_ref (FpcDisumUsbPrivate *priv)
{
  g_atomic_ref_count_inc (&priv->ref_count);

  return priv;
}

static FpcDisumUsbPrivate *
transport_priv (FpcDisumTransport *transport)
{
  return ((FpcDisumUsbTransport *)transport)->priv;
}

static void
op_free (FpcDisumUsbOp *op)
{
  if (op->retry_source != NULL)
    {
      g_source_destroy (op->retry_source);
      g_source_unref (op->retry_source);
      op->retry_source = NULL;
    }

  if (op->transfer != NULL)
    {
      libusb_free_transfer (op->transfer);
      op->transfer = NULL;
    }

  g_free (op->buffer);
  op->buffer = NULL;

  priv_unref (op->priv);
  g_free (op);
}

static const gchar *
usb_error_name (gint code)
{
  const gchar *name = libusb_error_name (code);

  return name != NULL ? name : "unknown libusb error";
}

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                                */
/* ------------------------------------------------------------------------- */

static void
diag_fill_common (
    FpcDisumUsbPrivate *priv, const FpcDisumUsbOp *op, FpcDisumDiagnosticRecord *record)
{
  memset (record, 0, sizeof *record);

  record->op_id = op->op_id;
  record->direction_valid = TRUE;
  record->direction
      = op->direction_in ? FPC_DISUM_DIAGNOSTIC_DIRECTION_IN : FPC_DISUM_DIAGNOSTIC_DIRECTION_OUT;
  record->control_valid = op->is_control;
  record->bm_request_type = op->bm_request_type;
  record->request = op->request;
  record->value = op->value;
  record->index = op->index;
  record->requested_length_valid = TRUE;
  record->requested_length = op->requested_length;

  if (priv->bound.valid)
    {
      guint8 count = MIN (priv->bound.port_count, FPC_DISUM_USB_MAX_PORTS);

      record->identity_valid = TRUE;
      record->generation = priv->generation;
      record->handle_id = priv->handle_id;
      record->bus = priv->bound.bus;
      record->port_count = count;
      memcpy (record->ports, priv->bound.ports, count);
      record->vid = priv->bound.vid;
      record->pid = priv->bound.pid;
      record->bcd_device = priv->bound.bcd_device;
      record->address = priv->bound.address;
    }
}

static void
diag_emit_submit (
    FpcDisumUsbPrivate *priv, const FpcDisumUsbOp *op, gboolean submitted, gint native)
{
  FpcDisumDiagnosticRecord record;

  if (priv->diagnostics == NULL)
    return;

  diag_fill_common (priv, op, &record);
  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_SUBMIT;
  record.t_us = op->submitted_us;
  record.submitted_valid = TRUE;
  record.submitted = submitted;
  if (!submitted)
    {
      record.native_valid = TRUE;
      record.native_status = native;
      record.native_domain = FPC_DISUM_DIAGNOSTIC_NATIVE_LIBUSB;
    }

  fpc_disum_diagnostics_emit (priv->diagnostics, &record);
}

static void
diag_emit_completion (FpcDisumUsbPrivate *priv, const FpcDisumUsbOp *op,
    FpcDisumDiagnosticKind kind, FpcDisumTransportStatus status, gint native, gboolean actual_known,
    gsize actual, gint64 t_us)
{
  FpcDisumDiagnosticRecord record;

  if (priv->diagnostics == NULL)
    return;

  diag_fill_common (priv, op, &record);
  record.kind = kind;
  record.t_us = t_us;
  record.status_valid = TRUE;
  record.status = status;
  record.native_valid = TRUE;
  record.native_status = native;
  record.native_domain = FPC_DISUM_DIAGNOSTIC_NATIVE_LIBUSB;
  record.actual_length_known = actual_known;
  record.actual_length = actual_known ? actual : 0;

  fpc_disum_diagnostics_emit (priv->diagnostics, &record);
}

static void
diag_emit_cancel_request (FpcDisumUsbPrivate *priv, const FpcDisumUsbOp *op, gint native)
{
  FpcDisumDiagnosticRecord record;

  if (priv->diagnostics == NULL)
    return;

  diag_fill_common (priv, op, &record);
  record.kind = FPC_DISUM_DIAGNOSTIC_KIND_CANCEL_REQUEST;
  record.native_valid = TRUE;
  record.native_status = native;
  record.native_domain = FPC_DISUM_DIAGNOSTIC_NATIVE_LIBUSB;

  fpc_disum_diagnostics_emit (priv->diagnostics, &record);
}

/* ------------------------------------------------------------------------- */
/* GSource over libusb's poll descriptors                                     */
/* ------------------------------------------------------------------------- */

static void
usb_source_add_pollfd (FpcDisumUsbPrivate *priv, int fd, short events)
{
  FpcDisumUsbSource *source = (FpcDisumUsbSource *)priv->source;
  GPollFD *pfd;
  guint i;

  if (source == NULL)
    return;

  for (i = 0; i < source->pollfds->len; i++)
    {
      pfd = g_ptr_array_index (source->pollfds, i);
      if (pfd->fd == fd)
        {
          pfd->events = events;
          return;
        }
    }

  pfd = g_new0 (GPollFD, 1);
  pfd->fd = fd;
  pfd->events = events;
  g_ptr_array_add (source->pollfds, pfd);
  g_source_add_poll (priv->source, pfd);
}

static void
usb_source_remove_pollfd (FpcDisumUsbPrivate *priv, int fd)
{
  FpcDisumUsbSource *source = (FpcDisumUsbSource *)priv->source;
  guint i;

  if (source == NULL)
    return;

  for (i = 0; i < source->pollfds->len; i++)
    {
      GPollFD *pfd = g_ptr_array_index (source->pollfds, i);

      if (pfd->fd == fd)
        {
          g_source_remove_poll (priv->source, pfd);
          g_ptr_array_remove_index (source->pollfds, i);
          return;
        }
    }
}

static void LIBUSB_CALL
pollfd_added_cb (int fd, short events, gpointer user_data)
{
  usb_source_add_pollfd (user_data, fd, events);
}

static void LIBUSB_CALL
pollfd_removed_cb (int fd, gpointer user_data)
{
  usb_source_remove_pollfd (user_data, fd);
}

static gboolean
usb_source_prepare (GSource *source, gint *timeout_)
{
  FpcDisumUsbSource *usb_source = (FpcDisumUsbSource *)source;
  struct timeval tv;

  usb_source->deadline_us = -1;

  if (libusb_get_next_timeout (usb_source->priv->context, &tv) == 1)
    {
      gint64 us = (gint64)tv.tv_sec * G_USEC_PER_SEC + tv.tv_usec;

      if (us < 0)
        us = 0;
      usb_source->deadline_us = g_get_monotonic_time () + us;
    }

  if (usb_source->deadline_us >= 0)
    {
      gint64 remaining = usb_source->deadline_us - g_get_monotonic_time ();
      gint ms = remaining <= 0 ? 0 : (gint)((remaining + 999) / 1000);

      if (*timeout_ < 0 || ms < *timeout_)
        *timeout_ = ms;
    }

  return FALSE;
}

static gboolean
usb_source_check (GSource *source)
{
  FpcDisumUsbSource *usb_source = (FpcDisumUsbSource *)source;
  FpcDisumUsbPrivate *priv = usb_source->priv;
  guint i;

  if (!g_queue_is_empty (&priv->completions))
    return TRUE;

  for (i = 0; i < usb_source->pollfds->len; i++)
    {
      GPollFD *pfd = g_ptr_array_index (usb_source->pollfds, i);

      if ((pfd->revents & (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL)) != 0)
        return TRUE;
    }

  if (usb_source->deadline_us >= 0 && g_get_monotonic_time () >= usb_source->deadline_us)
    return TRUE;

  return FALSE;
}

static gboolean
usb_source_dispatch (GSource *source, GSourceFunc callback, gpointer user_data)
{
  FpcDisumUsbSource *usb_source = (FpcDisumUsbSource *)source;
  FpcDisumUsbPrivate *priv = usb_source->priv;
  struct timeval zero = { 0, 0 };

  (void)callback;
  (void)user_data;

  /* Keep the private state alive for the whole delivery, including callbacks
   * that release the transport. */
  priv_ref (priv);

  libusb_handle_events_timeout_completed (priv->context, &zero, NULL);
  drain_completions (priv);

  priv_unref (priv);

  return G_SOURCE_CONTINUE;
}

static void
usb_source_finalize (GSource *source)
{
  FpcDisumUsbSource *usb_source = (FpcDisumUsbSource *)source;

  g_ptr_array_unref (usb_source->pollfds);
  usb_source->pollfds = NULL;
}

static GSourceFuncs usb_source_funcs = {
  .prepare = usb_source_prepare,
  .check = usb_source_check,
  .dispatch = usb_source_dispatch,
  .finalize = usb_source_finalize,
};

static GSource *
usb_source_new (FpcDisumUsbPrivate *priv)
{
  FpcDisumUsbSource *usb_source;
  GSource *source;

  source = g_source_new (&usb_source_funcs, sizeof (FpcDisumUsbSource));
  usb_source = (FpcDisumUsbSource *)source;
  usb_source->priv = priv;
  usb_source->pollfds = g_ptr_array_new_with_free_func (g_free);
  usb_source->deadline_us = -1;

  g_source_set_name (source, "fpc-disum-libusb");
  g_source_attach (source, priv->main_context);

  return source;
}

/* The initial descriptor set comes from libusb_get_pollfds(); later changes are
 * delivered by the notifiers, so the table is never rebuilt per prepare(). */
static void
usb_source_seed_pollfds (FpcDisumUsbPrivate *priv)
{
  const struct libusb_pollfd **pollfds;
  gsize i;

  pollfds = libusb_get_pollfds (priv->context);
  if (pollfds == NULL)
    return;

  for (i = 0; pollfds[i] != NULL; i++)
    usb_source_add_pollfd (priv, pollfds[i]->fd, pollfds[i]->events);

  libusb_free_pollfds (pollfds);
}

/* ------------------------------------------------------------------------- */
/* Status mapping                                                             */
/* ------------------------------------------------------------------------- */

static FpcDisumTransportStatus
status_from_transfer (enum libusb_transfer_status status)
{
  switch (status)
    {
    case LIBUSB_TRANSFER_COMPLETED:
      return FPC_DISUM_TRANSPORT_OK;
    case LIBUSB_TRANSFER_TIMED_OUT:
      return FPC_DISUM_TRANSPORT_TIMEOUT;
    case LIBUSB_TRANSFER_CANCELLED:
      return FPC_DISUM_TRANSPORT_ERROR_CANCELLED;
    case LIBUSB_TRANSFER_NO_DEVICE:
      return FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_STALL:
    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_OVERFLOW:
      return FPC_DISUM_TRANSPORT_ERROR_IO;
    }

  return FPC_DISUM_TRANSPORT_ERROR_IO;
}

static FpcDisumTransportStatus
status_from_submit_error (gint error)
{
  if (error == LIBUSB_ERROR_NO_DEVICE)
    return FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE;

  return FPC_DISUM_TRANSPORT_ERROR_IO;
}

/* The real payload length of a finished transfer.  For control transfers
 * libusb counts the 8-byte setup packet.  A length outside the operation's
 * capacity is not trusted. */
static gboolean
op_payload_length (const FpcDisumUsbOp *op, gsize *length)
{
  gint actual;

  if (op->transfer == NULL)
    return FALSE;

  actual = op->actual_length;
  if (actual < 0)
    return FALSE;

  /* libusb reports the data stage for control transfers; the setup packet is
   * not counted.  Zero-length commands therefore complete with 0. */
  *length = (gsize)actual;

  if (*length > op->requested_length)
    return FALSE;

  return TRUE;
}

static const guint8 *
op_payload (const FpcDisumUsbOp *op)
{
  if (op->buffer == NULL)
    return NULL;

  return op->buffer + (op->is_control ? LIBUSB_CONTROL_SETUP_SIZE : 0);
}

/* ------------------------------------------------------------------------- */
/* Submission and completion                                                  */
/* ------------------------------------------------------------------------- */

static void
op_queue_delivery (FpcDisumUsbPrivate *priv, FpcDisumUsbOp *op)
{
  g_queue_push_tail (&priv->completions, op);
  g_main_context_wakeup (priv->main_context);
}

/*
 * Claim the single in-flight slot.  The session serialises operations, so a
 * second concurrent operation is a caller error; it is still completed exactly
 * once, asynchronously, rather than vanishing.
 */
static gboolean
op_take_slot (FpcDisumUsbPrivate *priv, FpcDisumUsbOp *op)
{
  if (priv->torn_down)
    {
      op_free (op);
      return FALSE;
    }

  if (priv->in_flight != NULL)
    {
      g_warning ("FPC Disum transport: an operation is already in flight");
      op->slot_error = TRUE;
      op_queue_delivery (priv, op);
      return FALSE;
    }

  priv->in_flight = op;

  return TRUE;
}

/*
 * The libusb completion callback.  It only stores what libusb reported: no
 * caller callback, no synchronous probe, no close and no context teardown may
 * run from here.  Delivery happens after the event handler has returned.
 */
static void LIBUSB_CALL
transfer_completed_cb (struct libusb_transfer *transfer)
{
  FpcDisumUsbOp *op = transfer->user_data;

  if (op->completed)
    return;

  op->completed = TRUE;
  op->native_status = transfer->status;
  op->actual_length = transfer->actual_length;
  op->completed_us = g_get_monotonic_time ();

  g_queue_push_tail (&op->priv->completions, op);
}

static void
op_submit (FpcDisumUsbOp *op)
{
  FpcDisumUsbPrivate *priv = op->priv;
  gint result;

  op->op_id = priv->diagnostics != NULL ? fpc_disum_diagnostics_next_op_id (priv->diagnostics) : 0;
  op->submitted_us = g_get_monotonic_time ();
  op->completed_us = op->submitted_us;

  if (op->transfer == NULL || (op->kind != FPC_DISUM_USB_OP_REOPEN && priv->handle == NULL))
    {
      op->submit_error = LIBUSB_ERROR_OTHER;
      op->submitted = FALSE;
      diag_emit_submit (priv, op, FALSE, op->submit_error);
      op->completed = TRUE;
      op_queue_delivery (priv, op);
      return;
    }

  result = libusb_submit_transfer (op->transfer);
  op->submitted = result == 0;
  if (!op->submitted)
    op->submit_error = result;

  diag_emit_submit (priv, op, op->submitted, result);

  if (!op->submitted)
    {
      op->completed = TRUE;
      op_queue_delivery (priv, op);
    }
}

static FpcDisumUsbOp *
op_new (FpcDisumTransport *transport, FpcDisumUsbOpKind kind)
{
  FpcDisumUsbOp *op = g_new0 (FpcDisumUsbOp, 1);

  op->priv = priv_ref (transport_priv (transport));
  op->transport = transport;
  op->kind = kind;

  return op;
}

static void
deliver_transfer_op (FpcDisumUsbOp *op)
{
  FpcDisumUsbPrivate *priv = op->priv;
  FpcDisumTransportStatus status;
  gboolean actual_known = FALSE;
  gsize actual = 0;
  gint native;

  if (priv->in_flight == op)
    priv->in_flight = NULL;

  if (priv->torn_down)
    {
      op_free (op);
      return;
    }

  if (op->slot_error)
    {
      status = FPC_DISUM_TRANSPORT_ERROR_IO;
      native = LIBUSB_ERROR_BUSY;
    }
  else if (!op->submitted)
    {
      status = status_from_submit_error (op->submit_error);
      native = op->submit_error;
    }
  else
    {
      status = status_from_transfer ((enum libusb_transfer_status)op->native_status);
      native = op->native_status;
      actual_known = op_payload_length (op, &actual);
      if (!actual_known)
        {
          /* An impossible length makes a successful or timed-out transfer
           * untrustworthy; explicit failures keep their native status. */
          actual = 0;
          if (status == FPC_DISUM_TRANSPORT_OK || status == FPC_DISUM_TRANSPORT_TIMEOUT)
            status = FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN;
        }
    }

  if (op->cancel_requested)
    diag_emit_completion (priv, op, FPC_DISUM_DIAGNOSTIC_KIND_CANCEL_COMPLETE, status, native,
        actual_known, actual, op->completed_us);

  diag_emit_completion (priv, op, FPC_DISUM_DIAGNOSTIC_KIND_COMPLETE, status, native, actual_known,
      actual, op->completed_us);

  if (op->data_done != NULL)
    op->data_done (op->transport, status, actual_known ? op_payload (op) : NULL,
        actual_known ? actual : 0, op->user_data);
  else
    op->done (op->transport, status, actual_known ? actual : 0, op->user_data);

  op_free (op);
}

static void
drain_completions (FpcDisumUsbPrivate *priv)
{
  FpcDisumUsbOp *op;

  while ((op = g_queue_pop_head (&priv->completions)) != NULL)
    {
      if (op->kind == FPC_DISUM_USB_OP_REOPEN)
        reopen_transfer_completed (op);
      else
        {
          deliver_transfer_op (op);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Regular operations                                                         */
/* ------------------------------------------------------------------------- */

static void
fpc_disum_usb_control_out (FpcDisumTransport *transport, guint8 request, guint16 value,
    const guint8 *data, gsize len, FpcDisumTransportDoneFunc done, gpointer user_data)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);
  g_return_if_fail (len <= FPC_DISUM_USB_CONTROL_OUT_MAX);
  g_return_if_fail (len == 0 || data != NULL);

  priv = transport_priv (transport);

  op = op_new (transport, FPC_DISUM_USB_OP_CONTROL_OUT);
  op->is_control = TRUE;
  op->direction_in = FALSE;
  op->bm_request_type = FPC_DISUM_USB_BM_REQUEST_OUT;
  op->request = request;
  op->value = value;
  op->index = 0;
  op->requested_length = len;
  op->done = done;
  op->user_data = user_data;

  op->buffer_len = LIBUSB_CONTROL_SETUP_SIZE + len;
  op->buffer = g_malloc0 (op->buffer_len);
  libusb_fill_control_setup (
      op->buffer, FPC_DISUM_USB_BM_REQUEST_OUT, request, value, 0, (uint16_t)len);
  if (len > 0)
    memcpy (op->buffer + LIBUSB_CONTROL_SETUP_SIZE, data, len);

  if (!op_take_slot (priv, op))
    return;

  op->transfer = libusb_alloc_transfer (0);
  if (op->transfer != NULL)
    libusb_fill_control_transfer (op->transfer, priv->handle, op->buffer, transfer_completed_cb, op,
        FPC_DISUM_USB_CONTROL_TIMEOUT_MS);

  fp_dbg ("control out request 0x%02x (%zu B, timeout %u ms)", request, len,
      FPC_DISUM_USB_CONTROL_TIMEOUT_MS);

  op_submit (op);
}

static void
fpc_disum_usb_control_in (FpcDisumTransport *transport, guint8 request, guint16 value,
    gsize requested, FpcDisumTransportDataFunc done, gpointer user_data)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);
  g_return_if_fail (requested <= FPC_DISUM_USB_CONTROL_IN_MAX);

  priv = transport_priv (transport);

  op = op_new (transport, FPC_DISUM_USB_OP_CONTROL_IN);
  op->is_control = TRUE;
  op->direction_in = TRUE;
  op->bm_request_type = FPC_DISUM_USB_BM_REQUEST_IN;
  op->request = request;
  op->value = value;
  op->index = 0;
  op->requested_length = requested;
  op->data_done = done;
  op->user_data = user_data;

  op->buffer_len = LIBUSB_CONTROL_SETUP_SIZE + requested;
  op->buffer = g_malloc0 (op->buffer_len);
  libusb_fill_control_setup (
      op->buffer, FPC_DISUM_USB_BM_REQUEST_IN, request, value, 0, (uint16_t)requested);

  if (!op_take_slot (priv, op))
    return;

  op->transfer = libusb_alloc_transfer (0);
  if (op->transfer != NULL)
    libusb_fill_control_transfer (op->transfer, priv->handle, op->buffer, transfer_completed_cb, op,
        FPC_DISUM_USB_CONTROL_TIMEOUT_MS);

  fp_dbg ("control in request 0x%02x (capacity %zu B, timeout %u ms)", request, requested,
      FPC_DISUM_USB_CONTROL_TIMEOUT_MS);

  op_submit (op);
}

static void
fpc_disum_usb_bulk_read (
    FpcDisumTransport *transport, FpcDisumTransportDataFunc done, gpointer user_data)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);

  priv = transport_priv (transport);

  op = op_new (transport, FPC_DISUM_USB_OP_BULK_READ);
  op->is_control = FALSE;
  op->direction_in = TRUE;
  op->request = FPC_DISUM_USB_BULK_IN_EP;
  op->requested_length = FPC_DISUM_USB_BULK_READ_SIZE;
  op->data_done = done;
  op->user_data = user_data;
  op->buffer_len = FPC_DISUM_USB_BULK_READ_SIZE;
  op->buffer = g_malloc (op->buffer_len);

  if (!op_take_slot (priv, op))
    return;

  op->transfer = libusb_alloc_transfer (0);
  if (op->transfer != NULL)
    libusb_fill_bulk_transfer (op->transfer, priv->handle, FPC_DISUM_USB_BULK_IN_EP, op->buffer,
        FPC_DISUM_USB_BULK_READ_SIZE, transfer_completed_cb, op, FPC_DISUM_USB_BULK_TIMEOUT_MS);

  fp_dbg ("bulk read EP 0x%02x (capacity %u B, timeout %u ms)", FPC_DISUM_USB_BULK_IN_EP,
      FPC_DISUM_USB_BULK_READ_SIZE, FPC_DISUM_USB_BULK_TIMEOUT_MS);

  op_submit (op);
}

/* ------------------------------------------------------------------------- */
/* Raw handle lifetime and identity                                           */
/* ------------------------------------------------------------------------- */

static void
bound_from_raw (FpcDisumUsbPrivate *priv, libusb_device *raw)
{
  FpiUsbIdentity *identity = &priv->identity;
  guint count = MIN (identity->ports->len, FPC_DISUM_USB_MAX_PORTS);

  memset (&priv->bound, 0, sizeof priv->bound);
  priv->bound.valid = TRUE;
  priv->bound.bus = identity->bus;
  priv->bound.port_count = (guint8)count;
  memcpy (priv->bound.ports, identity->ports->data, count);
  priv->bound.vid = identity->vid;
  priv->bound.pid = identity->pid;
  priv->bound.bcd_device = identity->release;
  priv->bound.address = libusb_get_device_address (raw);

  priv->handle_id++;
}

static gboolean
raw_device_matches (const FpiUsbIdentity *identity, libusb_device *raw)
{
  struct libusb_device_descriptor descriptor;
  guint8 ports[FPC_DISUM_USB_MAX_PORTS];
  int port_count;

  if (identity->ports == NULL || identity->ports->len > FPC_DISUM_USB_MAX_PORTS)
    return FALSE;

  if (libusb_get_bus_number (raw) != identity->bus)
    return FALSE;

  port_count = libusb_get_port_numbers (raw, ports, FPC_DISUM_USB_MAX_PORTS);
  if (port_count < 0 || (guint)port_count != identity->ports->len)
    return FALSE;
  if (port_count > 0 && memcmp (ports, identity->ports->data, (gsize)port_count) != 0)
    return FALSE;

  if (libusb_get_device_descriptor (raw, &descriptor) != 0)
    return FALSE;

  return descriptor.idVendor == identity->vid && descriptor.idProduct == identity->pid
         && descriptor.bcdDevice == identity->release;
}

static void
close_raw_handle (libusb_device_handle **handle)
{
  if (*handle != NULL)
    {
      libusb_release_interface (*handle, FPC_DISUM_USB_INTERFACE);
      libusb_close (*handle);
      *handle = NULL;
    }
}

static void
close_bound_raw (FpcDisumUsbPrivate *priv)
{
  close_raw_handle (&priv->handle);

  if (priv->raw_device != NULL)
    {
      libusb_unref_device (priv->raw_device);
      priv->raw_device = NULL;
    }

  priv->bound.valid = FALSE;
}

static void
close_probe_raw (FpcDisumUsbOp *op)
{
  close_raw_handle (&op->probe_handle);

  if (op->probe_device != NULL)
    {
      libusb_unref_device (op->probe_device);
      op->probe_device = NULL;
    }
}

/* Open a candidate and claim interface 0.  The configuration is read and must
 * already be 1: SET_CONFIGURATION is never sent and the device is never reset. */
static gboolean
open_and_claim (libusb_device *raw, libusb_device_handle **handle_out, GError **error)
{
  libusb_device_handle *handle = NULL;
  int configuration = -1;
  int result;

  result = libusb_open (raw, &handle);
  if (result != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "opening the USB device failed: %s",
          usb_error_name (result));
      return FALSE;
    }

  result = libusb_claim_interface (handle, FPC_DISUM_USB_INTERFACE);
  if (result != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "claiming interface %d failed: %s",
          FPC_DISUM_USB_INTERFACE, usb_error_name (result));
      libusb_close (handle);
      return FALSE;
    }

  result = libusb_get_configuration (handle, &configuration);
  if (result != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "reading the USB configuration failed: %s",
          usb_error_name (result));
      libusb_release_interface (handle, FPC_DISUM_USB_INTERFACE);
      libusb_close (handle);
      return FALSE;
    }

  if (configuration != FPC_DISUM_USB_CONFIGURATION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "unexpected USB configuration %d (expected %d)", configuration,
          FPC_DISUM_USB_CONFIGURATION);
      libusb_release_interface (handle, FPC_DISUM_USB_INTERFACE);
      libusb_close (handle);
      return FALSE;
    }

  *handle_out = handle;

  return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Reopen after CMD_INIT                                                      */
/* ------------------------------------------------------------------------- */

static void
reopen_finish (FpcDisumUsbOp *op, FpcDisumTransportStatus status)
{
  FpcDisumUsbPrivate *priv = op->priv;

  if (priv->in_flight == op)
    priv->in_flight = NULL;

  if (priv->torn_down)
    {
      close_probe_raw (op);
      op_free (op);
      return;
    }

  op->done (op->transport, status, 0, op->user_data);
  op_free (op);
}

static void
reopen_reset_transfer (FpcDisumUsbOp *op)
{
  if (op->transfer != NULL)
    {
      libusb_free_transfer (op->transfer);
      op->transfer = NULL;
    }

  g_free (op->buffer);
  op->buffer = NULL;
  op->buffer_len = 0;
  op->submitted = FALSE;
  op->completed = FALSE;
  op->submit_error = 0;
  op->native_status = 0;
  op->actual_length = 0;
  op->submitted_us = 0;
  op->completed_us = 0;
}

static void
reopen_submit_probe (FpcDisumUsbOp *op, libusb_device_handle *handle)
{
  FpcDisumUsbPrivate *priv = op->priv;

  reopen_reset_transfer (op);

  op->is_control = TRUE;
  op->direction_in = TRUE;
  op->bm_request_type = FPC_DISUM_USB_BM_STANDARD_IN;
  op->request = FPC_DISUM_USB_REQ_GET_DESCRIPTOR;
  op->value = FPC_DISUM_USB_DESC_DEVICE;
  op->index = 0;
  op->requested_length = FPC_DISUM_USB_PROBE_BYTES;

  op->buffer_len = LIBUSB_CONTROL_SETUP_SIZE + FPC_DISUM_USB_PROBE_BYTES;
  op->buffer = g_malloc0 (op->buffer_len);
  libusb_fill_control_setup (op->buffer, FPC_DISUM_USB_BM_STANDARD_IN,
      FPC_DISUM_USB_REQ_GET_DESCRIPTOR, FPC_DISUM_USB_DESC_DEVICE, 0, FPC_DISUM_USB_PROBE_BYTES);

  op->transfer = libusb_alloc_transfer (0);
  if (op->transfer != NULL)
    libusb_fill_control_transfer (op->transfer, handle, op->buffer, transfer_completed_cb, op,
        FPC_DISUM_USB_CONTROL_TIMEOUT_MS);

  fp_dbg ("probing bus %u address %u with a device descriptor read", priv->bound.bus,
      priv->bound.address);

  op_submit (op);
}

static gboolean
probe_descriptor_matches (FpcDisumUsbPrivate *priv, const guint8 *descriptor, gsize length)
{
  guint16 vid, pid, bcd;

  if (length != FPC_DISUM_USB_PROBE_BYTES)
    return FALSE;

  if (descriptor[0] != FPC_DISUM_USB_PROBE_BYTES || descriptor[1] != 0x01)
    return FALSE;

  vid = (guint16)(descriptor[8] | (descriptor[9] << 8));
  pid = (guint16)(descriptor[10] | (descriptor[11] << 8));
  bcd = (guint16)(descriptor[12] | (descriptor[13] << 8));

  return vid == priv->identity.vid && pid == priv->identity.pid && bcd == priv->identity.release;
}

static gboolean
reopen_retry_cb (gpointer user_data)
{
  FpcDisumUsbOp *op = user_data;
  GSource *source = g_main_current_source ();

  op->retry_source = NULL;

  if (source != NULL)
    g_source_unref (source);

  reopen_attempt (op);

  return G_SOURCE_REMOVE;
}

static void
reopen_schedule (FpcDisumUsbOp *op)
{
  FpcDisumUsbPrivate *priv = op->priv;

  if (g_get_monotonic_time () >= op->deadline_us)
    {
      fp_dbg ("the device did not reappear before the reopen deadline");
      reopen_finish (op, FPC_DISUM_TRANSPORT_ERROR_IO);
      return;
    }

  op->retry_source = g_timeout_source_new (FPC_DISUM_USB_REOPEN_RETRY_MS);
  g_source_set_callback (op->retry_source, reopen_retry_cb, op, NULL);
  g_source_attach (op->retry_source, priv->main_context);
}

static void
reopen_attempt (FpcDisumUsbOp *op)
{
  FpcDisumUsbPrivate *priv = op->priv;
  libusb_device **list = NULL;
  libusb_device *candidate = NULL;
  ssize_t count;
  ssize_t i;
  g_autoptr (GError) error = NULL;

  if (priv->torn_down)
    {
      close_probe_raw (op);
      op_free (op);
      return;
    }

  if (op->cancel_requested)
    {
      reopen_finish (op, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);
      return;
    }

  if (g_get_monotonic_time () >= op->deadline_us)
    {
      fp_dbg ("the device did not reappear before the reopen deadline");
      reopen_finish (op, FPC_DISUM_TRANSPORT_ERROR_IO);
      return;
    }

  /* A new physical generation is looked up in a fresh list every attempt; the
   * old handle is released first so its interface claim cannot block the new
   * one. */
  close_bound_raw (priv);

  count = libusb_get_device_list (priv->context, &list);
  if (count < 0)
    {
      fp_dbg ("enumerating USB devices failed: %s", usb_error_name ((gint)count));
      reopen_schedule (op);
      return;
    }

  for (i = 0; i < count; i++)
    {
      if (raw_device_matches (&priv->identity, list[i]))
        {
          candidate = libusb_ref_device (list[i]);
          break;
        }
    }

  libusb_free_device_list (list, 1);
  list = NULL;

  if (candidate == NULL)
    {
      fp_dbg ("the device is not on the bus yet");
      reopen_schedule (op);
      return;
    }

  if (!open_and_claim (candidate, &op->probe_handle, &error))
    {
      fp_dbg ("reopening the candidate failed: %s",
          error != NULL ? error->message : "no error reported");
      libusb_unref_device (candidate);
      reopen_schedule (op);
      return;
    }

  op->probe_device = candidate;
  op->stage = FPC_DISUM_USB_REOPEN_PROBE_NEW;
  reopen_submit_probe (op, op->probe_handle);
}

static void
reopen_transfer_completed (FpcDisumUsbOp *op)
{
  FpcDisumUsbPrivate *priv = op->priv;
  FpcDisumTransportStatus status;
  gboolean actual_known = FALSE;
  gboolean alive = FALSE;
  gsize actual = 0;
  gint native;

  if (priv->torn_down)
    {
      if (priv->in_flight == op)
        priv->in_flight = NULL;
      close_probe_raw (op);
      op_free (op);
      return;
    }

  if (!op->submitted)
    {
      status = status_from_submit_error (op->submit_error);
      native = op->submit_error;
    }
  else
    {
      status = status_from_transfer ((enum libusb_transfer_status)op->native_status);
      native = op->native_status;
      actual_known = op_payload_length (op, &actual);
      if (!actual_known)
        {
          actual = 0;
          if (status == FPC_DISUM_TRANSPORT_OK || status == FPC_DISUM_TRANSPORT_TIMEOUT)
            status = FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN;
        }
    }

  if (status == FPC_DISUM_TRANSPORT_OK && actual_known && actual == FPC_DISUM_USB_PROBE_BYTES)
    alive = probe_descriptor_matches (priv, op_payload (op), actual);

  if (op->cancel_requested)
    diag_emit_completion (priv, op, FPC_DISUM_DIAGNOSTIC_KIND_CANCEL_COMPLETE, status, native,
        actual_known, actual, op->completed_us);

  diag_emit_completion (priv, op, FPC_DISUM_DIAGNOSTIC_KIND_COMPLETE, status, native, actual_known,
      actual, op->completed_us);

  if (op->cancel_requested)
    {
      close_probe_raw (op);
      reopen_finish (op, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);
      return;
    }

  if (op->stage == FPC_DISUM_USB_REOPEN_PROBE_OLD)
    {
      if (alive)
        {
          /* The device never left: the existing handle is still valid. */
          fp_dbg ("the current handle is still alive; keeping it");
          g_clear_object (&priv->core_candidate);
          if (fpi_device_usb_is_reenumeration_expected (priv->device))
            priv->core_candidate = fpi_device_usb_ref_reenumeration_candidate (priv->device);
          reopen_finish (op, FPC_DISUM_TRANSPORT_OK);
          return;
        }

      close_bound_raw (priv);
      op->stage = FPC_DISUM_USB_REOPEN_WAIT;
      reopen_schedule (op);
      return;
    }

  /* Probe of a freshly opened candidate. */
  if (alive)
    {
      priv->handle = op->probe_handle;
      priv->raw_device = op->probe_device;
      op->probe_handle = NULL;
      op->probe_device = NULL;

      /* The old handle, if any, was already closed before this attempt. */
      bound_from_raw (priv, priv->raw_device);
      g_clear_object (&priv->core_candidate);
      if (fpi_device_usb_is_reenumeration_expected (priv->device))
        priv->core_candidate = fpi_device_usb_ref_reenumeration_candidate (priv->device);

      fp_dbg ("reconnected: bus %u address %u, configuration %d, interface %d claimed",
          priv->bound.bus, priv->bound.address, FPC_DISUM_USB_CONFIGURATION,
          FPC_DISUM_USB_INTERFACE);

      reopen_finish (op, FPC_DISUM_TRANSPORT_OK);
      return;
    }

  close_probe_raw (op);
  op->stage = FPC_DISUM_USB_REOPEN_WAIT;
  reopen_schedule (op);
}

static void
fpc_disum_usb_reopen (FpcDisumTransport *transport, gint64 deadline_us,
    FpcDisumTransportDoneFunc done, gpointer user_data)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);

  priv = transport_priv (transport);

  op = op_new (transport, FPC_DISUM_USB_OP_REOPEN);
  op->done = done;
  op->user_data = user_data;
  op->deadline_us = deadline_us;
  op->stage = FPC_DISUM_USB_REOPEN_WAIT;

  if (!op_take_slot (priv, op))
    return;

  if (priv->handle != NULL)
    {
      /* The reconnect may not have involved a departure: keep the handle when
       * it still answers a standard descriptor request. */
      op->stage = FPC_DISUM_USB_REOPEN_PROBE_OLD;
      reopen_submit_probe (op, priv->handle);
      return;
    }

  reopen_attempt (op);
}

/* ------------------------------------------------------------------------- */
/* Public transport                                                           */
/* ------------------------------------------------------------------------- */

static void
fpc_disum_usb_cancel (FpcDisumTransport *transport)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbOp *op;
  gboolean had_retry;
  gint result = LIBUSB_SUCCESS;

  g_return_if_fail (transport != NULL);

  priv = transport_priv (transport);
  op = priv->in_flight;
  if (op == NULL || op->cancel_requested)
    return;

  op->cancel_requested = TRUE;

  had_retry = op->retry_source != NULL;
  if (had_retry)
    {
      g_source_destroy (op->retry_source);
      g_source_unref (op->retry_source);
      op->retry_source = NULL;
    }

  if (op->transfer != NULL && op->submitted && !op->completed)
    result = libusb_cancel_transfer (op->transfer);
  else
    result = LIBUSB_ERROR_NOT_FOUND;

  diag_emit_cancel_request (priv, op, result);

  /* A reopen that was waiting for its next attempt has no kernel transfer and
   * no queued completion: the retry source was the only thing left, so finish
   * it now.  A probe that is still in the kernel, or whose completion is
   * already queued, must complete through that real completion. */
  if (op->kind == FPC_DISUM_USB_OP_REOPEN && had_retry)
    reopen_finish (op, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);

  /* Otherwise libusb_complete() is the only thing that may complete the
   * operation; LIBUSB_ERROR_NOT_FOUND means the completion is already queued. */
}

static FpcDisumTransportStatus
fpc_disum_usb_init_rebind (
    FpcDisumTransport *transport, FpcDisumInitRebindAction action, gint64 deadline_us)
{
  FpcDisumUsbPrivate *priv;
  g_autoptr (GError) error = NULL;

  g_return_val_if_fail (transport != NULL, FPC_DISUM_TRANSPORT_ERROR_IO);

  priv = transport_priv (transport);

  switch (action)
    {
    case FPC_DISUM_INIT_REBIND_BEGIN:
      if (!fpi_device_usb_begin_reenumeration (priv->device, &error))
        {
          fp_dbg ("starting the initialisation rebind failed: %s",
              error != NULL ? error->message : "no error reported");
          return FPC_DISUM_TRANSPORT_ERROR_IO;
        }
      return FPC_DISUM_TRANSPORT_OK;

    case FPC_DISUM_INIT_REBIND_ALLOW:
      if (!fpi_device_usb_allow_reenumeration (priv->device, deadline_us, &error))
        {
          fp_dbg ("allowing the initialisation rebind failed: %s",
              error != NULL ? error->message : "no error reported");
          return FPC_DISUM_TRANSPORT_ERROR_IO;
        }
      return FPC_DISUM_TRANSPORT_OK;

    case FPC_DISUM_INIT_REBIND_COMMIT:
      {
        GUsbDevice *candidate;
        FpiUsbIdentity candidate_identity = { 0 };
        gboolean matches;

        candidate = fpi_device_usb_ref_reenumeration_candidate (priv->device);
        if (candidate == NULL)
          {
            fp_dbg ("no core candidate to commit the raw handle against");
            return FPC_DISUM_TRANSPORT_ERROR_IO;
          }

        if (priv->core_candidate != NULL && candidate != priv->core_candidate)
          {
            fp_dbg ("the core candidate changed after the raw handle was bound");
            g_object_unref (candidate);
            return FPC_DISUM_TRANSPORT_ERROR_IO;
          }

        if (!priv->bound.valid || priv->handle == NULL)
          {
            fp_dbg ("no raw handle is bound to commit");
            g_object_unref (candidate);
            return FPC_DISUM_TRANSPORT_ERROR_IO;
          }

        if (!fpi_usb_identity_from_device (candidate, &candidate_identity))
          {
            fp_dbg ("cannot read the core candidate identity");
            g_object_unref (candidate);
            return FPC_DISUM_TRANSPORT_ERROR_IO;
          }

        matches = fpi_usb_identity_matches_bus_ports (&candidate_identity, priv->bound.bus,
            priv->bound.ports, priv->bound.port_count, priv->bound.vid, priv->bound.pid,
            priv->bound.bcd_device);
        fpi_usb_identity_clear (&candidate_identity);

        if (!matches)
          {
            fp_dbg ("the bound raw handle does not match the core candidate");
            g_object_unref (candidate);
            return FPC_DISUM_TRANSPORT_ERROR_IO;
          }

        if (!fpi_device_usb_commit_reenumeration (priv->device, candidate, &error))
          {
            fp_dbg ("committing the initialisation rebind failed: %s",
                error != NULL ? error->message : "no error reported");
            g_object_unref (candidate);
            return FPC_DISUM_TRANSPORT_ERROR_IO;
          }

        g_object_unref (candidate);
        g_clear_object (&priv->core_candidate);
        return FPC_DISUM_TRANSPORT_OK;
      }

    case FPC_DISUM_INIT_REBIND_ABORT:
      fpi_device_usb_abort_reenumeration (priv->device);
      g_clear_object (&priv->core_candidate);
      return FPC_DISUM_TRANSPORT_OK;
    }

  return FPC_DISUM_TRANSPORT_ERROR_IO;
}

static void
fpc_disum_usb_identity (FpcDisumTransport *transport, FpcDisumTransportIdentity *out)
{
  FpcDisumUsbPrivate *priv;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (out != NULL);

  priv = transport_priv (transport);

  memset (out, 0, sizeof *out);
  out->generation = priv->generation;

  if (!priv->bound.valid)
    return;

  out->handle_id = priv->handle_id;
  out->bus = priv->bound.bus;
  out->port_count = MIN (priv->bound.port_count, FPC_DISUM_USB_MAX_PORTS);
  memcpy (out->ports, priv->bound.ports, out->port_count);
  out->vid = priv->bound.vid;
  out->pid = priv->bound.pid;
  out->bcd_device = priv->bound.bcd_device;
  out->address = priv->bound.address;
}

static void
priv_finalize (FpcDisumUsbPrivate *priv)
{
  if (priv->source != NULL)
    {
      if (priv->context != NULL && priv->notifiers_installed)
        libusb_set_pollfd_notifiers (priv->context, NULL, NULL, NULL);
      priv->notifiers_installed = FALSE;

      g_source_destroy (priv->source);
      g_source_unref (priv->source);
      priv->source = NULL;
    }

  close_bound_raw (priv);

  if (priv->context != NULL)
    {
      libusb_exit (priv->context);
      priv->context = NULL;
    }

  if (priv->main_context != NULL)
    {
      g_main_context_unref (priv->main_context);
      priv->main_context = NULL;
    }

  g_clear_object (&priv->core_candidate);

  if (priv->diagnostics != NULL)
    {
      fpc_disum_diagnostics_unref (priv->diagnostics);
      priv->diagnostics = NULL;
    }

  fpi_usb_identity_clear (&priv->identity);
  g_free (priv);
}

static void
priv_unref (FpcDisumUsbPrivate *priv)
{
  if (g_atomic_ref_count_dec (&priv->ref_count))
    priv_finalize (priv);
}

FpcDisumTransport *
fpc_disum_usb_transport_new (FpDevice *device, FpcDisumDiagnostics *diagnostics, GError **error)
{
  FpcDisumUsbPrivate *priv = NULL;
  FpcDisumUsbTransport *self = NULL;
  GUsbDevice *usb;
  FpiUsbIdentity identity = { 0 };
  libusb_context *context = NULL;
  libusb_device **list = NULL;
  libusb_device *raw_device = NULL;
  libusb_device_handle *handle = NULL;
  ssize_t count;
  ssize_t i;
  gint result;

  g_return_val_if_fail (device != NULL, NULL);

  usb = fpi_device_get_usb_device (device);
  if (usb == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "the device has no USB object");
      return NULL;
    }

  /* A synthesised descriptor must never be mapped onto a real device. */
  if (g_usb_device_is_emulated (usb))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "the USB device is emulated");
      return NULL;
    }

  if (!fpi_usb_identity_from_device (usb, &identity))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "the USB device identity is not usable");
      return NULL;
    }

  if (identity.ports == NULL || identity.ports->len > FPC_DISUM_USB_MAX_PORTS)
    {
      g_set_error (
          error, G_IO_ERROR, G_IO_ERROR_FAILED, "the USB port chain has an unsupported length");
      fpi_usb_identity_clear (&identity);
      return NULL;
    }

  result = libusb_init (&context);
  if (result != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "initialising libusb failed: %s",
          usb_error_name (result));
      fpi_usb_identity_clear (&identity);
      return NULL;
    }

  count = libusb_get_device_list (context, &list);
  if (count < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "enumerating USB devices failed: %s",
          usb_error_name ((gint)count));
      libusb_exit (context);
      fpi_usb_identity_clear (&identity);
      return NULL;
    }

  for (i = 0; i < count; i++)
    {
      if (raw_device_matches (&identity, list[i]))
        {
          raw_device = libusb_ref_device (list[i]);
          break;
        }
    }

  libusb_free_device_list (list, 1);
  list = NULL;

  if (raw_device == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no USB device matches the core identity");
      libusb_exit (context);
      fpi_usb_identity_clear (&identity);
      return NULL;
    }

  if (!open_and_claim (raw_device, &handle, error))
    {
      libusb_unref_device (raw_device);
      libusb_exit (context);
      fpi_usb_identity_clear (&identity);
      return NULL;
    }

  priv = g_new0 (FpcDisumUsbPrivate, 1);
  g_atomic_ref_count_init (&priv->ref_count);
  priv->device = device;
  priv->diagnostics = diagnostics != NULL ? fpc_disum_diagnostics_ref (diagnostics) : NULL;
  priv->context = context;
  priv->handle = handle;
  priv->raw_device = raw_device;
  priv->identity = identity;
  priv->generation = 0;
  g_queue_init (&priv->completions);

  bound_from_raw (priv, raw_device);

  priv->main_context = g_main_context_ref_thread_default ();
  priv->source = usb_source_new (priv);
  usb_source_seed_pollfds (priv);
  priv->notifiers_installed = TRUE;
  libusb_set_pollfd_notifiers (context, pollfd_added_cb, pollfd_removed_cb, priv);

  self = g_new0 (FpcDisumUsbTransport, 1);
  self->parent.control_out = fpc_disum_usb_control_out;
  self->parent.control_in = fpc_disum_usb_control_in;
  self->parent.bulk_read = fpc_disum_usb_bulk_read;
  self->parent.init_rebind = fpc_disum_usb_init_rebind;
  self->parent.reopen = fpc_disum_usb_reopen;
  self->parent.identity = fpc_disum_usb_identity;
  self->parent.cancel = fpc_disum_usb_cancel;
  self->priv = priv;

  return &self->parent;
}

void
fpc_disum_usb_transport_free (FpcDisumTransport *transport)
{
  FpcDisumUsbTransport *self;
  FpcDisumUsbPrivate *priv;

  if (transport == NULL)
    return;

  self = (FpcDisumUsbTransport *)transport;
  priv = self->priv;

  /* From now on no callback is delivered to the caller.  An operation whose
   * transfer is still owned by the kernel keeps the whole private state alive
   * until libusb reports its completion; only then are the handle, the raw
   * device, the source and the context released. */
  priv->torn_down = TRUE;

  if (priv->in_flight != NULL)
    {
      FpcDisumUsbOp *op = priv->in_flight;

      if (op->retry_source != NULL)
        {
          g_source_destroy (op->retry_source);
          g_source_unref (op->retry_source);
          op->retry_source = NULL;
        }

      if (op->transfer != NULL && op->submitted && !op->completed)
        {
          op->cancel_requested = TRUE;
          libusb_cancel_transfer (op->transfer);
          g_main_context_wakeup (priv->main_context);
        }
      else
        {
          /* No kernel I/O is outstanding for this operation. */
          if (op->kind == FPC_DISUM_USB_OP_REOPEN)
            close_probe_raw (op);
          g_queue_remove (&priv->completions, op);
          priv->in_flight = NULL;
          op_free (op);
        }
    }

  g_free (self);
  priv_unref (priv);
}
