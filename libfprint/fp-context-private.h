/*
 * Private accessors into FpContext for the USB hotplug fixture.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "fp-context.h"
#include <gusb.h>

G_BEGIN_DECLS

/* Borrowed; the context owns it and it lives as long as the context. */
GUsbContext *fpi_context_get_usb_context (FpContext *context);

G_END_DECLS
