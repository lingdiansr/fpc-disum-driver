/*
 * FPC Disum (10a5:9201) GUsb transport.
 *
 * Reference: docs/CLEANROOM_SPEC.md §3.1 (control channel), §3.2/§4.4 (bulk IN
 * frames and cross-packet aggregation), §6 step 4 (handle swap after CMD_INIT),
 * §16.2 (USB error handling).
 *
 * I/O goes through GUsb's asynchronous API instead of FpiUsbTransfer, because the
 * reviewed handle swap in §6 needs a handle the libfprint core does not own:
 * hardware evidence (2026-10-18) shows that closing the core's handle and opening
 * the same GUsbDevice object again does not yield a usable handle -- the device
 * drops off the bus and the kernel gives it a new address.  The swap therefore
 * looks the device up again in a context of our own and continues on that handle.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_usb.h"

#include <string.h>

#include <gio/gio.h>
#include <gusb.h>

#include "fpi-device.h"
#include "fpi-log.h"

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

/* SPEC §4.1: the largest control IN request is the key request (1000 B). */
#define FPC_DISUM_USB_CONTROL_IN_MAX 1000

/* SPEC §3.1: the control OUT data stage is at most 4 B. */
#define FPC_DISUM_USB_CONTROL_OUT_MAX 4

typedef struct _FpcDisumUsbOp FpcDisumUsbOp;

/*
 * The shared, reference-counted state.  It outlives the transport when a
 * transfer is still in flight so that GUsb's completion can run after free()
 * without touching freed memory (and without calling the caller's callback,
 * because torn_down is then set).
 */
typedef struct
{
  gatomicrefcount ref_count;
  FpDevice *device;
  FpcDisumUsbOp *in_flight;
  gboolean torn_down;

  /* Identity of the device we are talking to, for the §6 swap. */
  guint16 vid;
  guint16 pid;

  /* A context of our own, its device and the handle I/O currently uses. */
  GUsbContext *usb_context;
  GUsbDevice *usb_own;
  GUsbDevice *usb;

  /* One buffer per direction: the interface serialises operations (SPEC §3.2). */
  guint8 control_in[FPC_DISUM_USB_CONTROL_IN_MAX];
  guint8 control_out[FPC_DISUM_USB_CONTROL_OUT_MAX];
  guint8 bulk[FPC_DISUM_USB_BULK_PACKET_SIZE];
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
  FpcDisumTransport *transport;
  FpcDisumUsbOpKind kind;
  guint8 request;
  guint16 value;
  gsize length;
  FpcDisumTransportDoneFunc done;
  FpcDisumTransportDataFunc data_done;
  gpointer user_data;
  GCancellable *cancellable;
  gboolean cancelled;
  GSource *retry_source;
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

  if (op->cancelled && status == FPC_DISUM_TRANSPORT_OK)
    status = FPC_DISUM_TRANSPORT_ERROR_CANCELLED;

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
      || g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_CANCELLED)
      || g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE))
    return FPC_DISUM_TRANSPORT_ERROR_IO;

  return FPC_DISUM_TRANSPORT_ERROR_IO;
}

/*
 * Completion for the control and bulk transfers.  The actual length is reported
 * by the finish call, never assumed from the requested length (SPEC §3.1).
 */
static void
transfer_done_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  FpcDisumUsbOp *op = user_data;
  FpcDisumUsbPrivate *priv = op->priv;
  g_autoptr (GError) error = NULL;
  gssize transferred;

  if (op->kind == FPC_DISUM_USB_OP_CONTROL_OUT)
    transferred = g_usb_device_control_transfer_finish (G_USB_DEVICE (source_object), res, &error);
  else if (op->kind == FPC_DISUM_USB_OP_CONTROL_IN)
    transferred = g_usb_device_control_transfer_finish (G_USB_DEVICE (source_object), res, &error);
  else
    transferred = g_usb_device_bulk_transfer_finish (G_USB_DEVICE (source_object), res, &error);

  if (transferred < 0)
    {
      FpcDisumTransportStatus status = status_from_error (op, error);

      /* SPEC §3.2: a read that returned nothing -- including the poll timeout --
       * is OK with zero bytes, so the session can keep polling. */
      if (op->kind == FPC_DISUM_USB_OP_BULK_READ && error != NULL
          && g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          op_complete (op, FPC_DISUM_TRANSPORT_OK, NULL, 0);
          return;
        }

      if (error != NULL && status != FPC_DISUM_TRANSPORT_ERROR_CANCELLED)
        fp_dbg ("op %s request 0x%02x failed: %s", op_kind_name (op->kind), op->request,
            error->message);

      op_complete (op, status, NULL, 0);
      return;
    }

  if (op->kind == FPC_DISUM_USB_OP_CONTROL_IN)
    op_complete (op, FPC_DISUM_TRANSPORT_OK, priv->control_in, (gsize)transferred);
  else if (op->kind == FPC_DISUM_USB_OP_BULK_READ)
    op_complete (op, FPC_DISUM_TRANSPORT_OK, priv->bulk, (gsize)transferred);
  else
    op_complete (op, FPC_DISUM_TRANSPORT_OK, NULL, 0);
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
fpc_disum_usb_control_out (FpcDisumTransport *transport, guint8 request, guint16 value,
    const guint8 *data, gsize len, FpcDisumTransportDoneFunc done, gpointer user_data)
{
  FpcDisumUsbPrivate *priv = transport_priv (transport);
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);
  /* SPEC §3.1: the OUT data stage is at most 4 B. */
  g_return_if_fail (len <= FPC_DISUM_USB_CONTROL_OUT_MAX);
  g_return_if_fail (len == 0 || data != NULL);

  op = op_new (transport, FPC_DISUM_USB_OP_CONTROL_OUT);
  op->request = request;
  op->value = value;
  op->done = done;
  op->user_data = user_data;

  if (!op_start (op))
    return;

  if (len > 0)
    memcpy (priv->control_out, data, len);

  fp_dbg (
      "op control out request 0x%02x (timeout %u ms)", request, FPC_DISUM_USB_CONTROL_TIMEOUT_MS);

  g_usb_device_control_transfer_async (priv->usb, G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
      G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE, request, value, 0,
      len > 0 ? priv->control_out : NULL, len, FPC_DISUM_USB_CONTROL_TIMEOUT_MS, op->cancellable,
      transfer_done_cb, op);
}

static void
fpc_disum_usb_control_in (FpcDisumTransport *transport, guint8 request, guint16 value,
    gsize requested, FpcDisumTransportDataFunc done, gpointer user_data)
{
  FpcDisumUsbPrivate *priv = transport_priv (transport);
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);
  g_return_if_fail (requested <= sizeof priv->control_in);

  op = op_new (transport, FPC_DISUM_USB_OP_CONTROL_IN);
  op->request = request;
  op->value = value;
  op->length = requested;
  op->data_done = done;
  op->user_data = user_data;

  if (!op_start (op))
    return;

  fp_dbg ("op control in request 0x%02x (capacity %zu, timeout %u ms)", request, requested,
      FPC_DISUM_USB_CONTROL_TIMEOUT_MS);

  g_usb_device_control_transfer_async (priv->usb, G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
      G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE, request, value, 0,
      priv->control_in, requested, FPC_DISUM_USB_CONTROL_TIMEOUT_MS, op->cancellable,
      transfer_done_cb, op);
}

static void
fpc_disum_usb_bulk_read (
    FpcDisumTransport *transport, FpcDisumTransportDataFunc done, gpointer user_data)
{
  FpcDisumUsbPrivate *priv = transport_priv (transport);
  FpcDisumUsbOp *op;

  g_return_if_fail (transport != NULL);
  g_return_if_fail (done != NULL);

  op = op_new (transport, FPC_DISUM_USB_OP_BULK_READ);
  op->request = FPC_DISUM_USB_BULK_IN_EP;
  op->data_done = done;
  op->user_data = user_data;

  if (!op_start (op))
    return;

  fp_dbg ("op bulk read (timeout %u ms)", FPC_DISUM_USB_BULK_TIMEOUT_MS);

  g_usb_device_bulk_transfer_async (priv->usb, FPC_DISUM_USB_BULK_IN_EP, priv->bulk,
      sizeof priv->bulk, FPC_DISUM_USB_BULK_TIMEOUT_MS, op->cancellable, transfer_done_cb, op);
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
 * SPEC §6 step 4: after CMD_INIT the handle the core owns stops working.  The
 * device itself does not disappear from the bus for long, but the only way to
 * keep talking to it is a handle that is looked up and opened again -- hardware
 * evidence shows that reopening the core's own GUsbDevice object leaves an
 * unusable handle and knocks the device off the bus.  Our own context is used so
 * the core's object is never touched; SET_CONFIGURATION is never sent, because
 * the device has a single configuration that is already active (SPEC §16.2).
 */
static void
reopen_poll (FpcDisumUsbOp *op)
{
  FpcDisumUsbPrivate *priv = op->priv;
  g_autoptr (GError) error = NULL;
  GUsbDevice *found;
  gint current;

  if (op->cancelled)
    {
      op_complete (op, FPC_DISUM_TRANSPORT_ERROR_CANCELLED, NULL, 0);
      return;
    }

  if (priv->usb_own != NULL)
    {
      op_complete (op, FPC_DISUM_TRANSPORT_OK, NULL, 0);
      return;
    }

  if (priv->usb_context == NULL)
    {
      priv->usb_context = g_usb_context_new (&error);
      if (priv->usb_context == NULL)
        {
          fp_dbg ("creating our own USB context failed: %s",
              error != NULL ? error->message : "no error reported");
          op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
          return;
        }
    }

  g_usb_context_enumerate (priv->usb_context);
  found = g_usb_context_find_by_vid_pid (priv->usb_context, priv->vid, priv->pid, NULL);

  if (found == NULL)
    {
      /* Not visible again yet: retry until the deadline (SPEC §6 step 4). */
      if (g_get_monotonic_time () >= op->deadline_us)
        {
          fp_dbg ("the device did not become visible again after CMD_INIT");
          op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
          return;
        }

      op_reschedule (op);
      return;
    }

  if (!g_usb_device_open (found, &error))
    {
      fp_dbg ("opening the device again failed: %s",
          error != NULL ? error->message : "no error reported");
      if (g_get_monotonic_time () >= op->deadline_us)
        op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
      else
        op_reschedule (op);
      return;
    }

  current = g_usb_device_get_configuration (found, &error);
  if (current < 0)
    {
      fp_dbg ("reading the current configuration failed: %s",
          error != NULL ? error->message : "no error reported");
      g_usb_device_close (found, NULL);
      op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
      return;
    }

  if (!g_usb_device_claim_interface (found, FPC_DISUM_USB_INTERFACE, 0, &error))
    {
      fp_dbg ("claiming interface %d failed: %s", FPC_DISUM_USB_INTERFACE,
          error != NULL ? error->message : "no error reported");
      g_usb_device_close (found, NULL);
      op_complete (op, FPC_DISUM_TRANSPORT_ERROR_IO, NULL, 0);
      return;
    }

  fp_dbg ("reopened: new handle, configuration %d, interface %d claimed", current,
      FPC_DISUM_USB_INTERFACE);

  priv->usb_own = found;
  priv->usb = found;

  op_complete (op, FPC_DISUM_TRANSPORT_OK, NULL, 0);
}

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

  /* SPEC §6 step 4: the old handle is dead, so reopen_poll() looks the device up
   * again and switches priv->usb to that handle.  The core's object is left
   * untouched; it closes it when the device is closed. */
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

  /* GUsb completes the transfer with a cancelled error, which
   * transfer_done_cb() reports as FPC_DISUM_TRANSPORT_ERROR_CANCELLED. */
  g_cancellable_cancel (op->cancellable);
}

FpcDisumTransport *
fpc_disum_usb_transport_new (FpDevice *device)
{
  FpcDisumUsbPrivate *priv;
  FpcDisumUsbTransport *self;
  GUsbDevice *usb;

  g_return_val_if_fail (device != NULL, NULL);

  usb = fpi_device_get_usb_device (device);
  g_return_val_if_fail (usb != NULL, NULL);

  priv = g_new0 (FpcDisumUsbPrivate, 1);
  g_atomic_ref_count_init (&priv->ref_count);
  priv->device = device;
  priv->vid = g_usb_device_get_vid (usb);
  priv->pid = g_usb_device_get_pid (usb);
  /* Until the §6 swap, the handle the core opened is the one to use. */
  priv->usb = usb;

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
          /* GUsb still invokes the completion; the op and the shared state stay
           * alive until then. */
          g_cancellable_cancel (op->cancellable);
        }
    }

  /* Release the handle we opened ourselves; the core's handle is not ours. */
  if (priv->usb_own != NULL)
    {
      g_usb_device_close (priv->usb_own, NULL);
      priv->usb_own = NULL;
      priv->usb = NULL;
    }
  g_clear_object (&priv->usb_context);

  g_free (self);
  priv_unref (priv);
}
