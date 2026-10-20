/*
 * A USB FpDevice backed by a caller-supplied GUsbDevice, for the core hotplug
 * and libusb boundary tests.  It is not a fingerprint driver: probe does not
 * talk to the device, open may be completed by the test, and capture returns a
 * fixed artificial image plane.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gusb.h>

#include "fpi-device.h"

G_BEGIN_DECLS

/*
 * Create and initialise the fake USB device.  @usb is borrowed for the lifetime
 * of the device (the test owns it).  The call iterates the thread-default main
 * context until probe completes.  The returned object is owned by the caller.
 */
FpDevice *fpt_usb_device_new (GUsbDevice *usb);

G_END_DECLS
