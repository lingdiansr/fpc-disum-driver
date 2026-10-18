/*
 * FPC Disum (10a5:9201) GUsb transport.
 *
 * Reference: docs/CLEANROOM_SPEC.md §3.1 (control channel), §3.2/§4.4 (bulk IN
 * frames and cross-packet aggregation), §6 step 4 (handle swap after CMD_INIT),
 * §16.2 (USB error handling).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_usb.h"

#include <string.h>

#include <gio/gio.h>
#include <gusb.h>

#include "fpi-log.h"
#include "fpi-usb-transfer.h"

/* SPEC §1/§3.2: the only data endpoint is 0x82 IN BULK, maxPacketSize 64. */
#define FPC_DISUM_USB_BULK_IN_EP 0x82
#define FPC_DISUM_USB_BULK_PACKET_SIZE 64

/* SPEC §1: a single interface, num 0, alt 0. */
#define FPC_DISUM_USB_INTERFACE 0

/* SPEC §3.1: a command is a short vendor control transfer; a bounded timeout
 * keeps a stalled device from wedging the session. */
#define FPC_DISUM_USB_CONTROL_TIMEOUT_MS 5000

/* SPEC §3.2: a bulk IN read must complete with zero bytes when the device has
 * nothing to send, so the session can keep polling; this timeout bounds it. */
#define FPC_DISUM_USB_BULK_TIMEOUT_MS 1000

/* SPEC §6 step 4: the device is given up to 20 s to become visible again. */
#define FPC_DISUM_USB_REOPEN_TIMEOUT_MS 20000

/* Backoff between reopen attempts; the retry loop stays cancellable. */
#define FPC_DISUM_USB_REOPEN_RETRY_MS 50

typedef struct _FpcDisumUsbOp FpcDisumUsbOp;

/*
 * The shared, reference-counted state.  It outlives the transport when a
 * transfer is still in flight so that the library's completion callback can run
 * after free() without touching freed memory (and without calling the caller's
 * callback, because torn_down is then set).
 */
typedef struct
{
  gatomicrefcount ref_count;
  FpDevice *device;
  FpcDisumUsbOp *in_flight;
  gboolean torn_down;
} FpcDisumUsbPrivate;

typedef enum
{
  FPC_DISUM_USB_OP_CONTROL_OUT,
  FPC_DISUM_USB_OP_CONTROL_IN,
  FPC_DISUM_USB_OP_BULK_READ,
  FPC_DISUM_USB_OP_REOPEN,
} FpcDisumUsbOpKind;

static const gchar *
op_kind_name (guint kind)
{
  switch (kind)
    {
    case FPC_DISUM_USB_OP_CONTROL_OUT:
      return "control out";
    case FPC_DISUM_USB_OP_CONTROL_IN:
      return "control in";
    case FPC_DISUM_USB_OP_BULK_READ:
      return "bulk read";
    case FPC_DISUM_USB_OP_REOPEN:
      return "reopen";
    }

  return "unknown";
}

/* One operation; at most one is in flight at a time. */
struct _FpcDisumUsbOp
{
  FpcDisumUsbPrivate *priv;
  guint8 request;
  FpcDisumTransport *transport;
  FpcDisumUsbOpKind kind;
  FpcDisumTransportDoneFunc done;
  FpcDisumTransportDataFunc data_done;
  gpointer user_data;
  GCancellable *cancellable;
  gboolean cancelled;
  GSource *retry_source;
  gboolean usb_opened;
  gint64 deadline_us;
};

/* The public transport embeds the frozen vtable as its first member, mirroring
 * the offline device model in tests. */
typedef struct _FpcDisumUsbTransport
{
  FpcDisumTransport parent;
  FpcDisumUsbPrivate *priv;
} FpcDisumUsbTransport;

static FpcDisumUsbPrivate *
priv_ref (FpcDisumUsbPrivate *priv)
{
  g_atomic_ref_count_inc (&priv->ref_count);

  return priv;
}

static void
priv_unref (FpcDisumUsbPrivate *priv)
{
  if (g_atomic_ref_count_dec (&priv->ref_count))
    g_free (priv);
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
      op->retry_source = NULL;
    }

  g_clear_object (&op->cancellable);
  priv_unref (op->priv);
  g_free (op);
}

/*
 * Deliver one operation result and release it.  The in-flight slot is cleared
 * before the callee runs, so a callback may start the next operation.  The
 * callback is suppressed once the transport has been freed.
 */
static void
op_complete (FpcDisumUsbOp *op, FpcDisumTransportStatus status, const guint8 *data, gsize len)
{
  FpcDisumUsbPrivate *priv = op->priv;

  if (priv->in_flight == op)
    priv->in_flight = NULL;

  if (!priv->torn_down)
    {
      if (op->data_done != NULL)
        op->data_done (op->transport, status, data, len, op->user_data);
      else
        op->done (op->transport, status, op->user_data);
    }

  op_free (op);
}

static FpcDisumTransportStatus
status_from_error (const FpcDisumUsbOp *op, const GError *error)
{
  if (op->cancelled)
    return FPC_DISUM_TRANSPORT_ERROR_CANCELLED;

  if (error == NULL)
    return FPC_DISUM_TRANSPORT_OK;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
      || g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_CANCELLED))
    return FPC_DISUM_TRANSPORT_ERROR_CANCELLED;

  return FPC_DISUM_TRANSPORT_ERROR_IO;
}

static gsize
nonneg_length (gssize length)
{
  return length > 0 ? (gsize)length : 0;
}

static void
transfer_done_cb (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  FpcDisumUsbOp *op = user_data;
  FpcDisumTransportStatus status = status_from_error (op, error);

  (void)device;

  /* Keep the underlying reason: the caller only sees the transport status
   * (SPEC §16.2 asks for the original error stage and errno). */
  if (error != NULL && status != FPC_DISUM_TRANSPORT_ERROR_CANCELLED)
    fp_dbg ("op %u request 0x%02x failed: %s", op->kind, op->request, error->message);

  switch (op->kind)
    {
    case FPC_DISUM_USB_OP_CONTROL_OUT:
      op_complete (op, status, NULL, 0);
      break;

    case FPC_DISUM_USB_OP_CONTROL_IN:
      /* The requested capacity is not the returned length (SPEC §3.1). */
      if (status == FPC_DISUM_TRANSPORT_OK)
        op_complete (op, status, transfer->buffer, nonneg_length (transfer->actual_length));
      else
        op_complete (op, status, NULL, 0);
      break;

    case FPC_DISUM_USB_OP_BULK_READ:
      /* SPEC §3.2: a read that returned nothing — including the poll timeout —
       * is OK with len 0, so the session can keep polling. */
      if (status == FPC_DISUM_TRANSPORT_ERROR_CANCELLED)
        op_complete (op, status, NULL, 0);
      else if (error == NULL
               || g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        op_complete (
            op, FPC_DISUM_TRANSPORT_OK, transfer->buffer, nonneg_length (transfer->actual_length));
      else
        op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
      break;

    case FPC_DISUM_USB_OP_REOPEN:
    default:
      g_assert_not_reached ();
    }
}

static FpcDisumUsbOp *
op_new (FpcDisumTransport *transport, FpcDisumUsbOpKind kind)
{
  FpcDisumUsbOp *op = g_new0 (FpcDisumUsbOp, 1);

  op->priv = priv_ref (transport_priv (transport));
  op->transport = transport;
  op->kind = kind;
  op->cancellable = g_cancellable_new ();

  return op;
}

/*
 * Claim the single in-flight slot.  The interface serialises operations, so a
 * second concurrent operation is a caller error; it is reported as IO rather
 * than vanishing.
 */
static gboolean
op_start (FpcDisumUsbOp *op)
{
  if (op->priv->torn_down)
    {
      op_free (op);
      return FALSE;
    }

  if (op->priv->in_flight != NULL)
    {
      g_warning ("FPC Disum transport: an operation is already in flight");
      op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
      return FALSE;
    }

  op->priv->in_flight = op;

  return TRUE;
}

static void
start_transfer (FpcDisumUsbOp *op, FpiUsbTransfer *transfer, guint timeout_ms)
{
  fp_dbg ("op %s request 0x%02x (timeout %u ms)", op_kind_name (op->kind), op->request, timeout_ms);

  /* fpi_usb_transfer_submit() steals the transfer and calls the callback
   * exactly once, unref'ing the transfer afterwards. */
  fpi_usb_transfer_submit (
      g_steal_pointer (&transfer), timeout_ms, op->cancellable, transfer_done_cb, op);
}

static void
fpc_disum_usb_control_out (FpcDisumTransport *transport, guint8 request, guint16 value,
    const guint8 *data, gsize len, FpcDisumTransportDoneFunc done, gpointer user_data)
{
  g_autoptr (FpiUsbTransfer) transfer = NULL;
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);
  /* SPEC §3.1: the OUT data stage is at most 4 B. */
  g_return_if_fail (len <= 4);
  g_return_if_fail (len == 0 || data != NULL);

  op = op_new (transport, FPC_DISUM_USB_OP_CONTROL_OUT);
  op->request = request;
  op->done = done;
  op->user_data = user_data;

  if (!op_start (op))
    return;

  transfer = fpi_usb_transfer_new (op->priv->device);
  fpi_usb_transfer_fill_control (transfer, G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
      G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE, request, value, 0, len);
  if (len > 0)
    memcpy (transfer->buffer, data, len);

  start_transfer (op, g_steal_pointer (&transfer), FPC_DISUM_USB_CONTROL_TIMEOUT_MS);
}

static void
fpc_disum_usb_control_in (FpcDisumTransport *transport, guint8 request, guint16 value,
    gsize requested, FpcDisumTransportDataFunc done, gpointer user_data)
{
  g_autoptr (FpiUsbTransfer) transfer = NULL;
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);

  op = op_new (transport, FPC_DISUM_USB_OP_CONTROL_IN);
  op->request = request;
  op->data_done = done;
  op->user_data = user_data;

  if (!op_start (op))
    return;

  transfer = fpi_usb_transfer_new (op->priv->device);
  fpi_usb_transfer_fill_control (transfer, G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
      G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE, request, value, 0,
      requested);

  start_transfer (op, g_steal_pointer (&transfer), FPC_DISUM_USB_CONTROL_TIMEOUT_MS);
}

static void
fpc_disum_usb_bulk_read (
    FpcDisumTransport *transport, FpcDisumTransportDataFunc done, gpointer user_data)
{
  g_autoptr (FpiUsbTransfer) transfer = NULL;
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);

  op = op_new (transport, FPC_DISUM_USB_OP_BULK_READ);
  op->request = FPC_DISUM_USB_BULK_IN_EP;
  op->data_done = done;
  op->user_data = user_data;

  if (!op_start (op))
    return;

  transfer = fpi_usb_transfer_new (op->priv->device);
  fpi_usb_transfer_fill_bulk (transfer, FPC_DISUM_USB_BULK_IN_EP, FPC_DISUM_USB_BULK_PACKET_SIZE);

  start_transfer (op, g_steal_pointer (&transfer), FPC_DISUM_USB_BULK_TIMEOUT_MS);
}

static void reopen_poll (FpcDisumUsbOp *op);

static void
reopen_timeout_cb (FpDevice *device, gpointer user_data)
{
  (void)device;
  reopen_poll (user_data);
}

static void
op_reschedule (FpcDisumUsbOp *op)
{
  op->retry_source = fpi_device_add_timeout (
      op->priv->device, FPC_DISUM_USB_REOPEN_RETRY_MS, reopen_timeout_cb, op, NULL);
}

/*
 * SPEC §16.2: read the CURRENT configuration first and only switch when it is
 * not already the target; a redundant SET_CONFIGURATION makes bulk IN 0x82 fail
 * with EIO and can force a re-enumeration, so it is forbidden.  Then claim
 * interface 0; the reader is built per read, so a freshly claimed handle is a
 * usable reader again.
 */
static FpcDisumTransportStatus
reopen_configure (FpcDisumUsbOp *op)
{
  GUsbDevice *usb = fpi_device_get_usb_device (op->priv->device);
  GError *error = NULL;
  gint current;

  /*
   * SPEC §16.2: a redundant standard configuration request is forbidden -- it
   * makes bulk IN fail with EIO and can force a re-enumeration.  The device has a
   * single configuration (SPEC §1), which is already selected, so it is only read
   * and recorded here.
   */
  current = g_usb_device_get_configuration (usb, &error);
  if (current < 0)
    {
      fp_dbg ("reading the current configuration failed: %s",
          error != NULL ? error->message : "no error reported");
      g_clear_error (&error);
      return FPC_DISUM_TRANSPORT_ERROR_IO;
    }

  if (!g_usb_device_claim_interface (usb, FPC_DISUM_USB_INTERFACE, 0, &error))
    {
      fp_dbg ("claiming interface %d failed: %s", FPC_DISUM_USB_INTERFACE,
          error != NULL ? error->message : "no error reported");
      g_clear_error (&error);
      return FPC_DISUM_TRANSPORT_ERROR_IO;
    }

  fp_dbg ("reopened: configuration %d, interface %d claimed", current, FPC_DISUM_USB_INTERFACE);

  return FPC_DISUM_TRANSPORT_OK;
}

static void
reopen_poll (FpcDisumUsbOp *op)
{
  GUsbDevice *usb = fpi_device_get_usb_device (op->priv->device);

  if (op->cancelled)
    {
      op_complete (op, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, NULL, 0);
      return;
    }

  if (!op->usb_opened)
    {
      GError *error = NULL;

      if (!g_usb_device_open (usb, &error))
        {
          fp_dbg ("reopening the device failed: %s",
              error != NULL ? error->message : "no error reported");
          g_clear_error (&error);
          /* SPEC §6 step 4: retry while the device is not visible again, up to
           * the deadline, with a short cancellable backoff. */
          if (g_get_monotonic_time () >= op->deadline_us)
            op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
          else
            op_reschedule (op);
          return;
        }

      op->usb_opened = TRUE;
    }

  op_complete (op, reopen_configure (op), NULL, 0);
}

/*
 * SPEC §6 step 4: after CMD_INIT the old handle is dead and the device has to be
 * opened again, re-claimed and its reader rebuilt.
 *
 * Hardware evidence (2026-10-18, first M6 run): closing the core's handle and
 * opening the same GUsbDevice object again does NOT give a usable handle -- the
 * whole device drops off the bus, the kernel gives it a new address, and every
 * later transfer fails with EIO/ENODEV until it re-enumerates.  The reviewed
 * specification describes exactly that symptom as the result of a host-side
 * wrong action (SPEC §16.4).  So the swap must not touch the core's device
 * object: the transport needs its own freshly found handle (and its own I/O
 * path), which is the rework this comment marks.
 */
static void
fpc_disum_usb_reopen (
    FpcDisumTransport *transport, FpcDisumTransportDoneFunc done, gpointer user_data)
{
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);

  op = op_new (transport, FPC_DISUM_USB_OP_REOPEN);
  op->done = done;
  op->user_data = user_data;

  if (!op_start (op))
    return;

  op->deadline_us = g_get_monotonic_time () + (gint64)FPC_DISUM_USB_REOPEN_TIMEOUT_MS * 1000;

  /* SPEC §6 step 4: the old handle is dead after CMD_INIT.  Closing it once
   * here means every later attempt only reopens a fresh handle.  No reset, no
   * command and no fixed delay is used to make this succeed. */
  g_usb_device_close (fpi_device_get_usb_device (op->priv->device), NULL);

  reopen_poll (op);
}

static void
fpc_disum_usb_cancel (FpcDisumTransport *transport)
{
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);

  op = transport_priv (transport)->in_flight;
  if (op == NULL)
    return;

  op->cancelled = TRUE;

  if (op->kind == FPC_DISUM_USB_OP_REOPEN)
    {
      /* The retry loop has no transfer to cancel; stop the pending attempt. */
      if (op->retry_source != NULL)
        {
          g_source_destroy (op->retry_source);
          op->retry_source = NULL;
        }
      op_complete (op, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, NULL, 0);
      return;
    }

  /* The library completes the transfer with a cancelled error, which
   * transfer_done_cb() reports as FPC_DISUM_TRANSPORT_ERROR_CANCELLED. */
  g_cancellable_cancel (op->cancellable);
}

FpcDisumTransport *
fpc_disum_usb_transport_new (FpDevice *device)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbTransport *self;

  g_return_val_if_fail (device != NULL, NULL);

  priv = g_new0 (FpcDisumUsbPrivate, 1);
  g_atomic_ref_count_init (&priv->ref_count);
  priv->device = device;

  self = g_new0 (FpcDisumUsbTransport, 1);
  self->parent.control_out = fpc_disum_usb_control_out;
  self->parent.control_in = fpc_disum_usb_control_in;
  self->parent.bulk_read = fpc_disum_usb_bulk_read;
  self->parent.reopen = fpc_disum_usb_reopen;
  self->parent.cancel = fpc_disum_usb_cancel;
  self->priv = priv;

  return &self->parent;
}

void
fpc_disum_usb_transport_free (FpcDisumTransport *transport)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbTransport *self;

  if (transport == NULL)
    return;

  self = (FpcDisumUsbTransport *)transport;
  priv = self->priv;

  /* No callback may run after this returns: a transfer still in flight is
   * cancelled and its completion suppressed through torn_down, while a pending
   * reopen retry is stopped directly (no transfer will call back for it). */
  priv->torn_down = TRUE;

  if (priv->in_flight != NULL)
    {
      FpcDisumUsbOp *op = priv->in_flight;

      op->cancelled = TRUE;

      if (op->kind == FPC_DISUM_USB_OP_REOPEN)
        {
          if (op->retry_source != NULL)
            {
              g_source_destroy (op->retry_source);
              op->retry_source = NULL;
            }
          op_complete (op, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, NULL, 0);
        }
      else
        {
          /* The library still invokes transfer_done_cb(); the op and the
           * shared state stay alive until then. */
          g_cancellable_cancel (op->cancellable);
        }
    }

  g_free (self);
  priv_unref (priv);
}
