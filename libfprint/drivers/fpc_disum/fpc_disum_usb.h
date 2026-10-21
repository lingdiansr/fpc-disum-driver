/*
 * FPC Disum (10a5:9201) GUsb transport.
 *
 * Reference: docs/CLEANROOM_SPEC.md §3.1 (control channel), §3.2/§4.4 (bulk IN
 * frames and cross-packet aggregation), §6 step 4 (handle swap after CMD_INIT),
 * §16.2 (USB error handling).
 *
 * The reader lifecycle contract (start before CMD_INIT, queue across the reset,
 * reap before the handle swap, restart before the reopen callback) is documented
 * in docs/IMPLEMENTATION_REQUIREMENTS.md §2.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "fpi-device.h"
#include "fpc_disum_diagnostics.h"
#include "fpc_disum_transport.h"

G_BEGIN_DECLS

/* SPEC §1/§3.2: the only data endpoint carries at most maxPacketSize = 64 B. */
#define FPC_DISUM_USB_BULK_READ_SIZE 64

/*
 * Completed reads are buffered while no session read is waiting, so frames
 * that arrive before CMD_INIT (and across the reset window) are not lost.  The
 * bound is explicit: when it is reached the newest frame is dropped and
 * counted; the oldest data (the initialisation window) is kept.
 */
#define FPC_DISUM_USB_READ_QUEUE_MAX_CHUNKS 64
#define FPC_DISUM_USB_READ_QUEUE_MAX_BYTES                                                         \
  (FPC_DISUM_USB_READ_QUEUE_MAX_CHUNKS * FPC_DISUM_USB_BULK_READ_SIZE)

/*
 * A reopen keeps the old read URB outstanding while waiting for the reset to
 * terminate it naturally.  This bounds that wait; it is additionally capped by
 * the reopen deadline, so a read that only ever times out cannot stall reopen.
 */
#define FPC_DISUM_USB_READ_REAP_GRACE_MS 2500

/*
 * Create the libusb implementation of the transport boundary for @device.
 * @device must be a USB #FpDevice.  The transport borrows it for its whole
 * lifetime and uses the core's physical identity and reclaim transaction for
 * the CMD_INIT handle swap.
 */
FpcDisumTransport *fpc_disum_usb_transport_new (
    FpDevice *device, FpcDisumDiagnostics *diagnostics, GError **error);

/*
 * Release the transport.  A transfer that is still in flight is cancelled and
 * its callback is suppressed, so no callback fires after this returns; every
 * transfer is released by the time the transfer's completion runs.
 */
void fpc_disum_usb_transport_free (FpcDisumTransport *transport);

G_END_DECLS
