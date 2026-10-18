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

#pragma once

#include <glib.h>

#include "fpi-device.h"
#include "fpc_disum_transport.h"

G_BEGIN_DECLS

/*
 * Create the GUsb/FpiUsbTransfer implementation of the transport boundary for
 * @device.  @device must be a USB #FpDevice (the handle is taken from
 * fpi_device_get_usb_device()); the transport borrows it for its whole lifetime
 * and never touches the GUsbDevice's configuration or interface claiming
 * outside fpc_disum_usb_transport_reopen().
 */
FpcDisumTransport *fpc_disum_usb_transport_new (FpDevice *device);

/*
 * Release the transport.  A transfer that is still in flight is cancelled and
 * its callback is suppressed, so no callback fires after this returns; every
 * transfer is released by the time the transfer's completion runs.
 */
void fpc_disum_usb_transport_free (FpcDisumTransport *transport);

G_END_DECLS
