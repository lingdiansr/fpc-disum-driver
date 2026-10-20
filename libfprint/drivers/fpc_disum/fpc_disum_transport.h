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
  /* STALL, EIO, a failed control transfer or an unknown completion state. */
  FPC_DISUM_TRANSPORT_ERROR_IO,
  /* The operation was cancelled; any bytes already received are still reported. */
  FPC_DISUM_TRANSPORT_ERROR_CANCELLED,
  /* The transfer timed out; any bytes already received are still reported. */
  FPC_DISUM_TRANSPORT_TIMEOUT,
  /* The device left the bus. */
  FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE,
  /* The byte count cannot be trusted; the operation must fail and never be
   * turned into a zero-length read. */
  FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN,
} FpcDisumTransportStatus;

typedef struct _FpcDisumTransport FpcDisumTransport;

/*
 * The identity the transport actually bound.  generation is 0 while the core
 * cannot prove a new device generation; it is never invented from an address.
 * ports is the complete root-to-leaf chain (at most 8 entries).
 */
typedef struct
{
  guint64 generation;
  guint64 handle_id;
  guint8 bus;
  guint8 port_count;
  guint8 ports[8];
  guint16 vid;
  guint16 pid;
  guint16 bcd_device;
  guint8 address;
} FpcDisumTransportIdentity;

typedef enum
{
  /* CMD_INIT is about to be submitted: removals during this command are the
   * device's own doing, but a replacement may not be committed yet. */
  FPC_DISUM_INIT_REBIND_BEGIN,
  /* CMD_INIT completed with its 4 B payload: the 20 s reclaim window starts. */
  FPC_DISUM_INIT_REBIND_ALLOW,
  /* The reclaimed transport is about to send TLS_INIT. */
  FPC_DISUM_INIT_REBIND_COMMIT,
  /* The initialisation is giving up; idempotent. */
  FPC_DISUM_INIT_REBIND_ABORT,
} FpcDisumInitRebindAction;

typedef void (*FpcDisumTransportDoneFunc) (FpcDisumTransport *transport,
    FpcDisumTransportStatus status, gsize actual_length, gpointer user_data);
typedef void (*FpcDisumTransportDataFunc) (FpcDisumTransport *transport,
    FpcDisumTransportStatus status, const guint8 *data, gsize len, gpointer user_data);

/*
 * The session state machine drives the device only through this boundary, and the
 * ownership of transfers, handles and readers stays behind it.  Two implementations
 * exist: the libusb one used on hardware (fpc_disum_usb.c) and the scripted device
 * model used by the offline tests.
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
   * stage: 4 B for CMD_INIT/ARM and up to 64 B for CMD_TLS_DATA.  The session
   * enforces the per-command length; the transport reports the real actual_length
   * so a short completion cannot be mistaken for success.
   */
  void (*control_out) (FpcDisumTransport *transport, guint8 request, guint16 value,
      const guint8 *data, gsize len, FpcDisumTransportDoneFunc done, gpointer user_data);
  /*
   * Vendor control IN (bmRequestType 0xC0, wIndex 0).  @requested is the requested
   * capacity and is not the returned length; @len reports what the device returned.
   */
  void (*control_in) (FpcDisumTransport *transport, guint8 request, guint16 value, gsize requested,
      FpcDisumTransportDataFunc done, gpointer user_data);
  /*
   * One bulk IN read from EP 0x82.  A read may return any length up to the chunk;
   * a timed-out read reports FPC_DISUM_TRANSPORT_TIMEOUT with the bytes received
   * so far, and TIMEOUT with zero bytes is not an error.
   */
  void (*bulk_read) (
      FpcDisumTransport *transport, FpcDisumTransportDataFunc done, gpointer user_data);
  /*
   * The explicit CMD_INIT reclaim transaction.  Returns the transport status of
   * the transition; BEGIN and ALLOW are synchronous bookkeeping, COMMIT may fail
   * when the bound raw handle cannot be matched to the claimed candidate.
   */
  FpcDisumTransportStatus (*init_rebind) (
      FpcDisumTransport *transport, FpcDisumInitRebindAction action, gint64 deadline_us);
  /*
   * Reclaim the device after CMD_INIT within @deadline_us: wait for the complete
   * physical identity, open it, claim interface 0 and probe it.  A failure is
   * reported, never hidden behind the old handle.
   */
  void (*reopen) (FpcDisumTransport *transport, gint64 deadline_us, FpcDisumTransportDoneFunc done,
      gpointer user_data);
  /* Snapshot of what the transport is bound to right now. */
  void (*identity) (FpcDisumTransport *transport, FpcDisumTransportIdentity *out);
  /*
   * Cancel the operation that is currently in flight, if any: its callback then
   * completes with FPC_DISUM_TRANSPORT_ERROR_CANCELLED, possibly before cancel()
   * returns.  Calling this with no operation in flight does nothing.
   */
  void (*cancel) (FpcDisumTransport *transport);
};

G_END_DECLS
