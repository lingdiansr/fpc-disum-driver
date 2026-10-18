/*
 * FPC Disum (10a5:9201) transport boundary.
 *
 * Reference: docs/CLEANROOM_SPEC.md §3.1 (control channel), §3.2/§4.4 (bulk IN
 * frames and cross-packet aggregation), §6 step 4 (handle swap after CMD_INIT),
 * §16.2 (USB error handling).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  FPC_DISUM_TRANSPORT_OK = 0,
  /* STALL, EIO, ENODEV, disconnection or a failed control transfer (SPEC §16.2). */
  FPC_DISUM_TRANSPORT_ERROR_IO,
  /* The operation was cancelled; nothing was transferred. */
  FPC_DISUM_TRANSPORT_ERROR_CANCELLED,
} FpcDisumTransportStatus;

typedef struct _FpcDisumTransport FpcDisumTransport;

typedef void (*FpcDisumTransportDoneFunc) (
    FpcDisumTransport *transport, FpcDisumTransportStatus status, gpointer user_data);
typedef void (*FpcDisumTransportDataFunc) (FpcDisumTransport *transport,
    FpcDisumTransportStatus status, const guint8 *data, gsize len, gpointer user_data);

/*
 * The session state machine drives the device only through this boundary, and the
 * ownership of transfers, handles and readers stays behind it.  Two implementations
 * exist: the GUsb/FpiUsbTransfer one used on hardware (fpc_disum_usb.c) and the
 * scripted device model used by the offline tests.
 *
 * Every operation completes through exactly one callback, and the callee keeps
 * ownership of the buffers it passes back: the bytes are borrowed and are only
 * valid until the callback returns.  Operations are asynchronous; the caller always
 * receives its callback for an operation it started.
 */
struct _FpcDisumTransport
{
  /*
   * Vendor control OUT (bmRequestType 0x40, wIndex 0) carrying an optional data
   * stage: 4 B for CMD_INIT/ARM and up to 64 B for CMD_TLS_DATA, whose wLength is
   * variable (6-64 B; host->device TLS records are fragmented at 64 B) -- SPEC
   * §4.1/§8.  The session enforces the per-command length.
   */
  void (*control_out) (FpcDisumTransport *transport, guint8 request, guint16 value,
      const guint8 *data, gsize len, FpcDisumTransportDoneFunc done, gpointer user_data);
  /*
   * Vendor control IN (bmRequestType 0xC0, wIndex 0).  @requested is the requested
   * capacity and is not the returned length; @len reports what the device returned
   * (SPEC §3.1).
   */
  void (*control_in) (FpcDisumTransport *transport, guint8 request, guint16 value, gsize requested,
      FpcDisumTransportDataFunc done, gpointer user_data);
  /*
   * Declare that the device is expected to leave the bus and come back on its
   * own: CMD_INIT resets it and makes it re-enumerate with a new address
   * (SPEC §6 step 4).  A USB implementation passes that on to the core so the
   * self-inflicted departure is not mistaken for a removal; the expectation is
   * released when the reconnect finishes and when the transport is torn down, so
   * the session only has to raise it before the command.
   */
  void (*reenumeration_expected) (FpcDisumTransport *transport, gboolean expected);
  /*
   * One bulk IN read from EP 0x82.  A read may return any length up to the endpoint
   * packet size, and neither that length nor a short packet carries frame meaning
   * (SPEC §3.2/§4.4); a read that returns no bytes at all is reported as
   * FPC_DISUM_TRANSPORT_OK with @len 0 when the device has nothing to send.
   */
  void (*bulk_read) (
      FpcDisumTransport *transport, FpcDisumTransportDataFunc done, gpointer user_data);
  /*
   * SPEC §6 step 4: after CMD_INIT the old handle stops working, so the transport
   * closes it, opens the device again, re-claims the interface and rebuilds the
   * reader, waiting for the device to become visible again.  The target
   * configuration is read first and SET_CONFIGURATION is skipped when it is already
   * active (SPEC §16.2); no reset or fixed delay is used to make this succeed.
   */
  void (*reopen) (FpcDisumTransport *transport, FpcDisumTransportDoneFunc done, gpointer user_data);
  /*
   * Cancel the operation that is currently in flight, if any: its callback then
   * completes with FPC_DISUM_TRANSPORT_ERROR_CANCELLED, possibly before cancel()
   * returns.  Operations started afterwards are unaffected, so a caller may abandon
   * a stalled read and still send its cleanup commands.  Calling this with no
   * operation in flight does nothing.
   */
  void (*cancel) (FpcDisumTransport *transport);
};

G_END_DECLS
