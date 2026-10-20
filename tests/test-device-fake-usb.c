/*
 * A USB FpDevice backed by a caller-supplied GUsbDevice, for the core hotplug
 * and libusb boundary tests.  It is not a fingerprint driver: probe does not
 * talk to the device, open may be completed by the test, and capture returns a
 * fixed artificial image plane.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "fake_usb"

#include "fpi-log.h"
#include "fpi-device.h"

#include "test-device-fake-usb.h"

typedef struct
{
  FpDevice parent;
  gboolean defer_open;
} FptUsbDevice;

typedef struct
{
  FpDeviceClass parent_class;
} FptUsbDeviceClass;

GType fpt_usb_device_get_type (void);

#define FPT_TYPE_USB_DEVICE (fpt_usb_device_get_type ())
#define FPT_USB_DEVICE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FPT_TYPE_USB_DEVICE, FptUsbDevice))
#define FPT_IS_USB_DEVICE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FPT_TYPE_USB_DEVICE))

G_DEFINE_TYPE (FptUsbDevice, fpt_usb_device, FP_TYPE_DEVICE)

enum
{
  PROP_0,
  PROP_DEFER_OPEN,
  N_PROPS,
};

static GParamSpec *properties[N_PROPS];

static const FpIdEntry driver_ids[] = {
  { .pid = 0x9201, .vid = 0x10a5 },
  { .pid = 0, .vid = 0 },
};

static void
fpt_usb_device_probe (FpDevice *device)
{
  g_assert_cmpuint (fpi_device_get_current_action (device), ==,
                    FPI_DEVICE_ACTION_PROBE);

  /* Deliberately no USB traffic: probe only declares the artificial identity. */
  fpi_device_update_features (device, FP_DEVICE_FEATURE_CAPTURE,
                              FP_DEVICE_FEATURE_CAPTURE);
  fpi_device_probe_complete (device, "fake_usb", "Fake USB device", NULL);
}

static void
fpt_usb_device_open (FpDevice *device)
{
  FptUsbDevice *self = FPT_USB_DEVICE (device);

  g_assert_cmpuint (fpi_device_get_current_action (device), ==,
                    FPI_DEVICE_ACTION_OPEN);

  if (!self->defer_open)
    fpi_device_open_complete (device, NULL);
}

static void
fpt_usb_device_close (FpDevice *device)
{
  g_assert_cmpuint (fpi_device_get_current_action (device), ==,
                    FPI_DEVICE_ACTION_CLOSE);

  fpi_device_close_complete (device, NULL);
}

static void
fpt_usb_device_capture (FpDevice *device)
{
  g_autoptr(FpImage) image = NULL;
  gboolean wait_for_finger;

  g_assert_cmpuint (fpi_device_get_current_action (device), ==,
                    FPI_DEVICE_ACTION_CAPTURE);

  fpi_device_get_capture_data (device, &wait_for_finger);

  image = fp_image_new (8, 8);
  fpi_device_capture_complete (device, g_steal_pointer (&image), NULL);
}

static void
fpt_usb_device_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  FptUsbDevice *self = FPT_USB_DEVICE (object);

  switch (prop_id)
    {
    case PROP_DEFER_OPEN:
      g_value_set_boolean (value, self->defer_open);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
fpt_usb_device_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  FptUsbDevice *self = FPT_USB_DEVICE (object);

  switch (prop_id)
    {
    case PROP_DEFER_OPEN:
      self->defer_open = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
fpt_usb_device_init (FptUsbDevice *self)
{
  self->defer_open = FALSE;
}

static void
fpt_usb_device_class_init (FptUsbDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  object_class->get_property = fpt_usb_device_get_property;
  object_class->set_property = fpt_usb_device_set_property;

  /**
   * FptUsbDevice:defer-open:
   *
   * When set before #fp_device_open(), the driver does not complete the open
   * operation; the test completes it with fpi_device_open_complete().
   */
  properties[PROP_DEFER_OPEN] =
    g_param_spec_boolean ("defer-open", NULL, NULL, FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, properties);

  dev_class->id = "fake_usb";
  dev_class->full_name = "Fake USB device";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = driver_ids;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  dev_class->probe = fpt_usb_device_probe;
  dev_class->open = fpt_usb_device_open;
  dev_class->close = fpt_usb_device_close;
  dev_class->capture = fpt_usb_device_capture;

  fpi_device_class_auto_initialize_features (dev_class);
}

typedef struct
{
  GMainLoop *loop;
  FpDevice  *device;
  GError    *error;
  gboolean   done;
} InitData;

static void
init_done_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  InitData *data = user_data;

  data->device = FP_DEVICE (g_async_initable_new_finish (
                              G_ASYNC_INITABLE (source_object), res,
                              &data->error));
  data->done = TRUE;
  g_main_loop_quit (data->loop);
}

FpDevice *
fpt_usb_device_new (GUsbDevice *usb)
{
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GMainContext) context = NULL;
  InitData data = { 0 };

  g_return_val_if_fail (G_USB_IS_DEVICE (usb), NULL);

  context = g_main_context_ref_thread_default ();
  loop = g_main_loop_new (context, FALSE);
  data.loop = loop;

  g_async_initable_new_async (fpt_usb_device_get_type (),
                              G_PRIORITY_DEFAULT,
                              NULL,
                              init_done_cb,
                              &data,
                              "fpi-usb-device", usb,
                              NULL);

  if (!data.done)
    g_main_loop_run (loop);

  if (data.error != NULL)
    g_error ("Failed to initialise the fake USB device: %s",
             data.error->message);

  return data.device;
}
