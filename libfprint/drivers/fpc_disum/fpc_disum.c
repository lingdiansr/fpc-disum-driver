/*
 * FPC Disum (10a5:9201) image driver.
 *
 * Reference: docs/CLEANROOM_SPEC.md §1 (identity and firmware), §4.5 (single
 * capture), §6 (initialisation), §8 (security channel), §16 (failure,
 * cancellation and cleanup).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <string.h>

#include "config.h"

#include "fpi-device.h"
#include "fpi-image.h"
#include "fpi-image-device.h"

#include "fpc_disum_diagnostics.h"
#include "fpc_disum_error.h"
#include "fpc_disum_proto.h"
#include "fpc_disum_session.h"
#include "fpc_disum_usb.h"

#define FP_COMPONENT "fpc_disum"

/* SPEC §1: the reviewed device.  Reverse engineering other devices of the same
 * vendor, or another firmware revision of this one, is out of scope: they are
 * refused rather than assumed compatible. */
#define FPC_DISUM_VID 0x10a5
#define FPC_DISUM_PID 0x9201
#define FPC_DISUM_BCD_DEVICE 0x0250

typedef enum
{
  /* No session is running. */
  FPC_DISUM_STATE_IDLE,
  /* SPEC §6 + §8 is running. */
  FPC_DISUM_STATE_OPENING,
  /* The session is open; a capture may be activated. */
  FPC_DISUM_STATE_READY,
  /* SPEC §4.5 capture is running. */
  FPC_DISUM_STATE_CAPTURING,
  /* SPEC §16.3 cleanup is running. */
  FPC_DISUM_STATE_CLOSING,
} FpcDisumState;

struct _FpiDeviceFpcDisum
{
  FpImageDevice parent;

  /* One session per open (SPEC §16.3); the transport belongs to it. */
  FpcDisumTransport *transport;
  FpcDisumSession *session;
  FpcDisumState state;

  /* The core asked to close while an operation was running. */
  gboolean close_pending;
  /* The core asked to deactivate while a capture was running. */
  gboolean deactivate_pending;
  /* The initialisation failed and the operation being reported is the open. */
  gboolean opening_failed;

  /* Cancellation during open, where the image-device core sends no deactivate. */
  GCancellable *open_cancellable;
  gulong open_cancellable_id;

  /* De-sensitised completion records for one device (all opens share the run).
   * The driver only forwards them; the probe owns all evidence files. */
  FpcDisumDiagnostics *diagnostics;
  guint diagnostic_signal_id;
};

G_DECLARE_FINAL_TYPE (
    FpiDeviceFpcDisum, fpi_device_fpc_disum, FPI, DEVICE_FPC_DISUM, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceFpcDisum, fpi_device_fpc_disum, FP_TYPE_IMAGE_DEVICE);

/* ------------------------------------------------------------- completion */

/* Finish the current action at most once; @status must not be OK. */
static void
dev_complete_failure (FpiDeviceFpcDisum *self, FpcDisumSessionStatus status, const gchar *message)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (self);
  g_autoptr (GError) error = fpc_disum_error_from_status (status, message);

  g_assert (error != NULL);

  if (self->opening_failed || self->state == FPC_DISUM_STATE_OPENING)
    {
      self->opening_failed = FALSE;
      fpi_image_device_open_complete (dev, g_steal_pointer (&error));
      return;
    }

  if (self->deactivate_pending)
    {
      self->deactivate_pending = FALSE;
      fpi_image_device_deactivate_complete (dev, g_steal_pointer (&error));
      return;
    }

  fpi_image_device_session_error (dev, g_steal_pointer (&error));
}

/* --------------------------------------------------------------- cleanup */

static void dev_start_close (FpiDeviceFpcDisum *self);
static void dev_close_done (FpcDisumSessionStatus status, const gchar *message, gpointer user_data);

static void
dev_start_close (FpiDeviceFpcDisum *self)
{
  self->state = FPC_DISUM_STATE_CLOSING;
  self->close_pending = FALSE;
  self->deactivate_pending = FALSE;

  fpc_disum_session_close_async (self->session, dev_close_done, self);
}

static void
dev_close_done (FpcDisumSessionStatus status, const gchar *message, gpointer user_data)
{
  FpiDeviceFpcDisum *self = user_data;
  FpImageDevice *dev = FP_IMAGE_DEVICE (self);
  /* The cleanup report carries the first root cause, so a cleanup failure never
   * hides the original error (SPEC §16.3). */
  g_autoptr (GError) error = fpc_disum_error_from_status (status, message);

  fpc_disum_session_free (self->session);
  self->session = NULL;
  g_clear_pointer (&self->transport, fpc_disum_usb_transport_free);
  self->state = FPC_DISUM_STATE_IDLE;

  if (self->opening_failed)
    {
      self->opening_failed = FALSE;
      fpi_image_device_open_complete (dev, g_steal_pointer (&error));
      return;
    }

  fpi_image_device_close_complete (dev, g_steal_pointer (&error));
}

/* ------------------------------------------------------- capture handling */

static void
dev_capture_done (const FpcDisumCaptureResult *result, gpointer user_data)
{
  FpiDeviceFpcDisum *self = user_data;
  FpImageDevice *dev = FP_IMAGE_DEVICE (self);
  gboolean deactivate = self->deactivate_pending;

  g_assert (self->state == FPC_DISUM_STATE_CAPTURING);

  self->deactivate_pending = FALSE;

  /* The core asked to stop: report that instead of delivering a result. */
  if (deactivate)
    {
      g_autoptr (GError) error
          = result->status == FPC_DISUM_SESSION_ERROR_CANCELLED
                ? NULL
                : fpc_disum_error_from_status (result->status, result->message);

      self->state = FPC_DISUM_STATE_READY;
      fpi_image_device_deactivate_complete (dev, g_steal_pointer (&error));

      if (self->close_pending)
        dev_start_close (self);

      return;
    }

  /* The core is closing the device: the outcome is carried by the close report,
   * which keeps the first root cause (SPEC §16.3). */
  if (self->close_pending)
    {
      self->state = FPC_DISUM_STATE_READY;
      dev_start_close (self);
      return;
    }

  if (result->status != FPC_DISUM_SESSION_OK)
    {
      self->state = FPC_DISUM_STATE_READY;
      dev_complete_failure (self, result->status, result->message);
      return;
    }

  {
    g_autoptr (FpImage) image = fp_image_new (FPC_DISUM_IMAGE_WIDTH, FPC_DISUM_IMAGE_HEIGHT);

    self->state = FPC_DISUM_STATE_READY;

    if (image == NULL)
      {
        dev_complete_failure (
            self, FPC_DISUM_SESSION_ERROR_IO, "The captured image could not be allocated");
        return;
      }

    memcpy (image->data, result->image, result->image_len);
    fpi_image_device_image_captured (dev, g_steal_pointer (&image));
    fpi_image_device_report_finger_status (dev, FALSE);
  }

  if (self->close_pending)
    dev_start_close (self);
}

static void
dev_touch (gpointer user_data)
{
  FpiDeviceFpcDisum *self = user_data;

  /* The device saw the finger; the image request follows immediately (SPEC §4.5). */
  fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (self), TRUE);
}

/* ARM completed with its four payload bytes; the session is now armed and
 * waiting for the touch.  Only a capture may be announced: an ARM during
 * initialisation or cleanup is never a cue for the operator. */
static void
dev_arm_complete (gsize actual_length, gpointer user_data)
{
  FpiDeviceFpcDisum *self = user_data;

  if (self->state != FPC_DISUM_STATE_CAPTURING)
    return;

  g_signal_emit_by_name (self, "arm-complete", (guint)actual_length);
}

/* Forward one de-sensitised record to subscribers.  The variant is built only
 * when someone listens, so an unattended run pays nothing for it. */
static void
dev_diagnostic (const FpcDisumDiagnosticRecord *record, gpointer user_data)
{
  FpiDeviceFpcDisum *self = user_data;
  g_autoptr (GVariant) variant = NULL;

  if (self->diagnostics == NULL || self->diagnostic_signal_id == 0)
    return;

  if (!g_signal_has_handler_pending (FP_DEVICE (self), self->diagnostic_signal_id, 0, FALSE))
    return;

  /* to_variant() returns a floating reference; sink it so the GValue made by
   * the emitter and this scope each own a reference. */
  variant = fpc_disum_diagnostics_to_variant (self->diagnostics, record);
  if (variant == NULL)
    return;

  g_variant_ref_sink (variant);

  g_signal_emit (FP_DEVICE (self), self->diagnostic_signal_id, 0, variant);
}

/* ---------------------------------------------------------- core callbacks */

static void
dev_cancelled (GCancellable *cancellable, gpointer user_data)
{
  FpiDeviceFpcDisum *self = user_data;

  /* Stop the running §6/§8 work; it completes with a cancellation. */
  if (self->session != NULL)
    fpc_disum_session_cancel (self->session);
}

static void
dev_open_done (FpcDisumSessionStatus status, const gchar *message, gpointer user_data)
{
  FpiDeviceFpcDisum *self = user_data;
  FpImageDevice *dev = FP_IMAGE_DEVICE (self);

  if (self->open_cancellable_id != 0)
    {
      g_cancellable_disconnect (self->open_cancellable, self->open_cancellable_id);
      self->open_cancellable_id = 0;
    }
  g_clear_object (&self->open_cancellable);

  if (status == FPC_DISUM_SESSION_OK)
    {
      self->state = FPC_DISUM_STATE_READY;
      fpi_image_device_open_complete (dev, NULL);

      if (self->close_pending)
        dev_start_close (self);

      return;
    }

  /* The device is left clean even when opening failed: the same cleanup runs and
   * the open carries the original error (SPEC §16.3). */
  self->opening_failed = TRUE;
  dev_start_close (self);
}

/* ---------------------------------------------------------- device actions */

static void
dev_open (FpImageDevice *dev)
{
  FpiDeviceFpcDisum *self = FPI_DEVICE_FPC_DISUM (dev);
  g_autoptr (GError) error = NULL;

  g_assert (self->state == FPC_DISUM_STATE_IDLE);

  self->state = FPC_DISUM_STATE_OPENING;
  self->transport = fpc_disum_usb_transport_new (FP_DEVICE (dev), self->diagnostics, &error);

  if (self->transport == NULL)
    {
      self->state = FPC_DISUM_STATE_IDLE;

      if (error == NULL)
        error = fpi_device_error_new_msg (
            FP_DEVICE_ERROR_GENERAL, "The device transport could not be created");

      fpi_image_device_open_complete (dev, g_steal_pointer (&error));
      return;
    }

  self->session = fpc_disum_session_new (self->transport);
  fpc_disum_session_set_diagnostics (self->session, self->diagnostics);
  fpc_disum_session_set_touch_handler (self->session, dev_touch, self);
  fpc_disum_session_set_arm_complete_handler (self->session, dev_arm_complete, self);

  /* An open has no deactivate to rely on, so a cancellation has to reach the
   * initialisation directly. */
  self->open_cancellable = fpi_device_get_cancellable (FP_DEVICE (dev));
  if (self->open_cancellable != NULL)
    {
      g_object_ref (self->open_cancellable);
      self->open_cancellable_id
          = g_cancellable_connect (self->open_cancellable, G_CALLBACK (dev_cancelled), self, NULL);
    }

  fpc_disum_session_init_async (self->session, dev_open_done, self);
}

static void
dev_close (FpImageDevice *dev)
{
  FpiDeviceFpcDisum *self = FPI_DEVICE_FPC_DISUM (dev);

  g_assert (self->state != FPC_DISUM_STATE_CLOSING);

  self->close_pending = TRUE;

  if (self->session == NULL)
    {
      /* Nothing was started, or the session was released already. */
      self->close_pending = FALSE;
      fpi_image_device_close_complete (dev, NULL);
      return;
    }

  if (self->state == FPC_DISUM_STATE_OPENING || self->state == FPC_DISUM_STATE_CAPTURING)
    {
      /* Let the running operation end first; its completion runs the cleanup
       * (SPEC §16.3). */
      self->deactivate_pending = FALSE;
      fpc_disum_session_cancel (self->session);
      return;
    }

  dev_start_close (self);
}

static void
dev_activate (FpImageDevice *dev)
{
  FpiDeviceFpcDisum *self = FPI_DEVICE_FPC_DISUM (dev);

  if (self->state != FPC_DISUM_STATE_READY)
    {
      fpi_image_device_activate_complete (
          dev, fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_OPEN, "The device is not open"));
      return;
    }

  /* Ready to capture; the work starts when the core reports the finger state. */
  fpi_image_device_activate_complete (dev, NULL);
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceFpcDisum *self = FPI_DEVICE_FPC_DISUM (dev);

  if (self->state == FPC_DISUM_STATE_CAPTURING)
    {
      /* A capture is running: stop it and complete from its callback. */
      self->deactivate_pending = TRUE;
      fpc_disum_session_cancel (self->session);
      return;
    }

  if (self->close_pending)
    return;

  fpi_image_device_deactivate_complete (dev, NULL);
}

static void
dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceFpcDisum *self = FPI_DEVICE_FPC_DISUM (dev);

  if (state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    return;

  if (self->state != FPC_DISUM_STATE_READY)
    return;

  self->state = FPC_DISUM_STATE_CAPTURING;
  fpc_disum_session_capture_async (self->session, dev_capture_done, self);
}

/* ------------------------------------------------------------- device glue */

static gint
dev_usb_discover (GUsbDevice *usb_device)
{
  /* Only the reviewed firmware revision is our device (SPEC §1).  The video and
   * the other descriptors are already implied by the id table entry. */
  if (g_usb_device_get_release (usb_device) != FPC_DISUM_BCD_DEVICE)
    return 0;

  return 100;
}

static const FpIdEntry id_table[] = {
  {
      .vid = FPC_DISUM_VID,
      .pid = FPC_DISUM_PID,
  },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

/* Private read-only properties, deliberately not part of the public ABI: the
 * probe binds its evidence to the build and the instance it actually loaded. */
enum
{
  PROP_0,
  PROP_FPC_DISUM_RUN_ID,
  PROP_FPC_DISUM_SPEC_SHA256,
};

static void
fpi_device_fpc_disum_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  FpiDeviceFpcDisum *self = FPI_DEVICE_FPC_DISUM (object);

  switch (prop_id)
    {
    case PROP_FPC_DISUM_RUN_ID:
      g_value_set_string (value,
          self->diagnostics != NULL ? fpc_disum_diagnostics_run_id (self->diagnostics) : NULL);
      break;

    case PROP_FPC_DISUM_SPEC_SHA256:
      /* Build-time identity of the reviewed specification (config.h). */
      g_value_set_string (value, FPC_DISUM_REVIEWED_SPEC_SHA256);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
fpi_device_fpc_disum_finalize (GObject *object)
{
  FpiDeviceFpcDisum *self = FPI_DEVICE_FPC_DISUM (object);

  if (self->open_cancellable_id != 0)
    {
      g_cancellable_disconnect (self->open_cancellable, self->open_cancellable_id);
      self->open_cancellable_id = 0;
    }
  g_clear_object (&self->open_cancellable);

  /* Drop the observer before anything it could still reach is released, so no
   * completion record can call back into a dying FpDevice. */
  if (self->diagnostics != NULL)
    fpc_disum_diagnostics_set_observer (self->diagnostics, NULL, NULL);

  g_clear_pointer (&self->session, fpc_disum_session_free);
  g_clear_pointer (&self->transport, fpc_disum_usb_transport_free);
  g_clear_pointer (&self->diagnostics, fpc_disum_diagnostics_unref);

  G_OBJECT_CLASS (fpi_device_fpc_disum_parent_class)->finalize (object);
}

static void
fpi_device_fpc_disum_init (FpiDeviceFpcDisum *self)
{
  self->diagnostics = fpc_disum_diagnostics_new ();
  self->diagnostic_signal_id = g_signal_lookup ("fpc-disum-diagnostic", G_OBJECT_TYPE (self));

  fpc_disum_diagnostics_set_observer (self->diagnostics, dev_diagnostic, self);
}

static void
fpi_device_fpc_disum_class_init (FpiDeviceFpcDisumClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  object_class->finalize = fpi_device_fpc_disum_finalize;
  object_class->get_property = fpi_device_fpc_disum_get_property;

  g_object_class_install_property (object_class, PROP_FPC_DISUM_RUN_ID,
      g_param_spec_string ("fpi-fpc-disum-run-id", "Run identifier",
          "Diagnostic run identifier shared by every open of this device", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_FPC_DISUM_SPEC_SHA256,
      g_param_spec_string ("fpi-fpc-disum-spec-sha256", "Reviewed specification",
          "SHA-256 of the cleanroom specification this driver was built against", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /* One G_TYPE_UINT: the control transfer's actual length (four on success).
   * The library never plays a sound or spawns a notification; the operator
   * probe is the only consumer. */
  g_signal_new ("arm-complete", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_UINT);

  /* One a{sv} of whitelisted metadata; never any I/O payload. */
  g_signal_new ("fpc-disum-diagnostic", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 1, G_TYPE_VARIANT);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "FPC Disum Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->usb_discover = dev_usb_discover;

  /* SPEC §5: the reviewed geometry; the physical orientation is a convention and
   * the plane is never rotated, transposed or flipped. */
  img_class->img_width = FPC_DISUM_IMAGE_WIDTH;
  img_class->img_height = FPC_DISUM_IMAGE_HEIGHT;

  img_class->img_open = dev_open;
  img_class->img_close = dev_close;
  img_class->activate = dev_activate;
  img_class->change_state = dev_change_state;
  img_class->deactivate = dev_deactivate;
}
