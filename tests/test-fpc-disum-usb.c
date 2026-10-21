/*
 * Offline tests for the libusb transport boundary.
 *
 * The production fpc_disum_usb.c is compiled into this executable unchanged;
 * every libusb entry point it uses is redirected at link time to a fake
 * context/device/handle whose only poll descriptor is a local pipe.  The fake
 * still drives the real GLib GSource and the real completion path, so the
 * status and transferred length observed by the caller are the ones libusb
 * would report.  No real USB device is touched and GUsb's own libusb use is
 * unaffected (only references from this executable are wrapped).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gusb.h>
#include <json-glib/json-glib.h>
#include <libusb.h>

#include "fpi-device.h"
#include "fpc_disum_diagnostics.h"
#include "fpc_disum_proto.h"
#include "fpc_disum_transport.h"
#include "fpc_disum_usb.h"
#include "test-device-fake-usb.h"

/* ------------------------------------------------------------------------- */
/* Fake libusb                                                                */
/* ------------------------------------------------------------------------- */

typedef struct
{
  struct libusb_device_descriptor descriptor;
  guint8 bus;
  guint8 address;
  guint8 ports[8];
  guint port_count;
  gint refs;
} FakeDevice;

typedef struct
{
  FakeDevice *device;
} FakeHandle;

typedef struct
{
  gboolean pipe_open;
  int pipefd[2];

  GQueue *pending; /* struct libusb_transfer * */

  libusb_pollfd_added_cb added_cb;
  libusb_pollfd_removed_cb removed_cb;
  gpointer notifier_data;

  gint configuration;
  gint open_error;
  gint claim_error;
  gint submit_error;

  gint init_count;
  gint exit_count;
  gint open_calls;
  gint claim_calls;
  gint release_calls;
  gint close_calls;
  gint alloc_calls;
  gint free_calls;
  gint submit_calls;
  gint cancel_calls;
  gint events_calls;
  gint list_calls;
} FakeUsb;

static FakeUsb fake;
static FakeDevice fake_device;

void __wrap_libusb_unref_device (libusb_device *device);
static struct libusb_pollfd fake_pollfd_entries[2];
static const struct libusb_pollfd *fake_pollfd_list[2];

static void
fake_reset (void)
{
  if (fake.pending != NULL)
    {
      g_queue_free (fake.pending);
      fake.pending = NULL;
    }

  if (fake.pipe_open)
    {
      close (fake.pipefd[0]);
      close (fake.pipefd[1]);
    }

  memset (&fake, 0, sizeof fake);
  memset (&fake_device, 0, sizeof fake_device);

  if (pipe2 (fake.pipefd, O_NONBLOCK | O_CLOEXEC) != 0)
    g_error ("pipe2 failed: %s", g_strerror (errno));

  fake.pipe_open = TRUE;
  fake.pending = g_queue_new ();
  fake.configuration = 1;
}

static void
fake_wake (void)
{
  const guint8 byte = 0x01;

  if (fake.pipe_open)
    (void)write (fake.pipefd[1], &byte, 1);
}

static void
fake_drain (void)
{
  guint8 buffer[64];

  while (read (fake.pipefd[0], buffer, sizeof buffer) > 0)
    ;
}

int
__wrap_libusb_init (libusb_context **context)
{
  g_assert_nonnull (context);
  fake.init_count++;
  *context = (libusb_context *)&fake;

  return 0;
}

void
__wrap_libusb_exit (libusb_context *context)
{
  (void)context;
  fake.exit_count++;

  if (fake.pipe_open)
    {
      close (fake.pipefd[0]);
      close (fake.pipefd[1]);
      fake.pipe_open = FALSE;
    }
}

ssize_t
__wrap_libusb_get_device_list (libusb_context *context, libusb_device ***list)
{
  struct libusb_device **devices;

  (void)context;
  fake.list_calls++;

  devices = g_new0 (struct libusb_device *, 2);
  devices[0] = (struct libusb_device *)&fake_device;
  fake_device.refs++;
  *list = devices;

  return 1;
}

void
__wrap_libusb_free_device_list (libusb_device **list, int unref_devices)
{
  gsize i;

  if (list == NULL)
    return;

  if (unref_devices)
    {
      for (i = 0; list[i] != NULL; i++)
        __wrap_libusb_unref_device (list[i]);
    }

  g_free (list);
}

libusb_device *
__wrap_libusb_ref_device (libusb_device *device)
{
  FakeDevice *self = (FakeDevice *)device;

  self->refs++;

  return device;
}

void
__wrap_libusb_unref_device (libusb_device *device)
{
  FakeDevice *self = (FakeDevice *)device;

  g_assert_cmpint (self->refs, >, 0);
  self->refs--;
}

uint8_t
__wrap_libusb_get_bus_number (libusb_device *device)
{
  return ((FakeDevice *)device)->bus;
}

uint8_t
__wrap_libusb_get_device_address (libusb_device *device)
{
  return ((FakeDevice *)device)->address;
}

int
__wrap_libusb_get_port_numbers (libusb_device *device, uint8_t *ports, int length)
{
  FakeDevice *self = (FakeDevice *)device;

  if ((gint)self->port_count > length)
    return LIBUSB_ERROR_OVERFLOW;

  if (self->port_count > 0)
    memcpy (ports, self->ports, self->port_count);

  return (int)self->port_count;
}

int
__wrap_libusb_get_device_descriptor (
    libusb_device *device, struct libusb_device_descriptor *descriptor)
{
  *descriptor = ((FakeDevice *)device)->descriptor;

  return 0;
}

int
__wrap_libusb_open (libusb_device *device, libusb_device_handle **handle)
{
  FakeHandle *self;

  fake.open_calls++;

  if (fake.open_error != 0)
    return fake.open_error;

  self = g_new0 (FakeHandle, 1);
  self->device = (FakeDevice *)device;
  *handle = (libusb_device_handle *)self;

  return 0;
}

void
__wrap_libusb_close (libusb_device_handle *handle)
{
  fake.close_calls++;
  g_free ((FakeHandle *)handle);
}

int
__wrap_libusb_get_configuration (libusb_device_handle *handle, int *configuration)
{
  (void)handle;
  *configuration = fake.configuration;

  return 0;
}

int
__wrap_libusb_claim_interface (libusb_device_handle *handle, int interface_number)
{
  (void)handle;
  (void)interface_number;
  fake.claim_calls++;

  return fake.claim_error;
}

int
__wrap_libusb_release_interface (libusb_device_handle *handle, int interface_number)
{
  (void)handle;
  (void)interface_number;
  fake.release_calls++;

  return 0;
}

struct libusb_transfer *
__wrap_libusb_alloc_transfer (int iso_packets)
{
  fake.alloc_calls++;

  return g_malloc0 (sizeof (struct libusb_transfer)
                    + (gsize)iso_packets * sizeof (struct libusb_iso_packet_descriptor));
}

void
__wrap_libusb_free_transfer (struct libusb_transfer *transfer)
{
  fake.free_calls++;
  g_queue_remove (fake.pending, transfer);
  g_free (transfer);
}

int
__wrap_libusb_submit_transfer (struct libusb_transfer *transfer)
{
  fake.submit_calls++;

  if (fake.submit_error != 0)
    return fake.submit_error;

  g_queue_push_tail (fake.pending, transfer);

  return 0;
}

int
__wrap_libusb_cancel_transfer (struct libusb_transfer *transfer)
{
  fake.cancel_calls++;

  if (!g_queue_find (fake.pending, transfer))
    return LIBUSB_ERROR_NOT_FOUND;

  return 0;
}

const struct libusb_pollfd **
__wrap_libusb_get_pollfds (libusb_context *context)
{
  (void)context;

  if (!fake.pipe_open)
    return NULL;

  fake_pollfd_entries[0].fd = fake.pipefd[0];
  fake_pollfd_entries[0].events = POLLIN;
  fake_pollfd_list[0] = &fake_pollfd_entries[0];
  fake_pollfd_list[1] = NULL;

  return fake_pollfd_list;
}

void
__wrap_libusb_free_pollfds (const struct libusb_pollfd **pollfds)
{
  (void)pollfds;
}

void
__wrap_libusb_set_pollfd_notifiers (libusb_context *context, libusb_pollfd_added_cb added_cb,
    libusb_pollfd_removed_cb removed_cb, gpointer user_data)
{
  (void)context;

  fake.added_cb = added_cb;
  fake.removed_cb = removed_cb;
  fake.notifier_data = user_data;

  if (added_cb != NULL && fake.pipe_open)
    added_cb (fake.pipefd[0], POLLIN, user_data);
}

int
__wrap_libusb_get_next_timeout (libusb_context *context, struct timeval *timeout)
{
  (void)context;
  (void)timeout;

  return 0;
}

int
__wrap_libusb_handle_events_timeout_completed (
    libusb_context *context, struct timeval *timeout, int *completed)
{
  (void)context;
  (void)timeout;
  (void)completed;

  fake.events_calls++;
  fake_drain ();

  return 0;
}

gboolean
__wrap_g_usb_device_is_emulated (GUsbDevice *device)
{
  (void)device;

  return FALSE;
}

/* Complete a fake transfer.  The production callback only records the result
 * and queues delivery; the caller callback runs during the next source
 * dispatch. */
static void
fake_complete (
    struct libusb_transfer *transfer, enum libusb_transfer_status status, int actual_length)
{
  g_assert_nonnull (transfer);
  g_assert_true (g_queue_remove (fake.pending, transfer));

  transfer->status = status;
  transfer->actual_length = actual_length;
  transfer->callback (transfer);
  fake_wake ();
}

static void
fake_complete_control (struct libusb_transfer *transfer, enum libusb_transfer_status status,
    const guint8 *payload, gsize payload_len)
{
  if (payload != NULL && payload_len > 0)
    memcpy (transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE, payload, payload_len);

  fake_complete (transfer, status, (int)payload_len);
}

static struct libusb_transfer *
fake_pending_of_type (guint8 type)
{
  GList *item;

  for (item = fake.pending->head; item != NULL; item = item->next)
    {
      struct libusb_transfer *transfer = item->data;

      if (transfer->type == type)
        return transfer;
    }

  return NULL;
}

/* The backend keeps exactly one read URB outstanding on EP 0x82. */
static struct libusb_transfer *
fake_pending_read (void)
{
  struct libusb_transfer *transfer = fake_pending_of_type (LIBUSB_TRANSFER_TYPE_BULK);

  g_assert_true (transfer == NULL || transfer->endpoint == 0x82);

  return transfer;
}

static struct libusb_transfer *
fake_pending_control (void)
{
  return fake_pending_of_type (LIBUSB_TRANSFER_TYPE_CONTROL);
}

/* After the transport is freed every remaining transfer was cancelled; deliver
 * that completion so the fake resource counters settle exactly once. */
static void pump_main_context (void);

static void
fake_reap_pending (void)
{
  while (fake.pending != NULL && !g_queue_is_empty (fake.pending))
    {
      struct libusb_transfer *transfer = g_queue_peek_head (fake.pending);

      fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 0);
      pump_main_context ();
    }
}

/* ------------------------------------------------------------------------- */
/* Fixture                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct
{
  GUsbContext *usb_context;
  GUsbDevice *usb;
  FpDevice *device;
  FpcDisumDiagnostics *diagnostics;
  FpcDisumTransport *transport;
} Fixture;

static void
pump_main_context (void)
{
  guint i;

  for (i = 0; i < 64; i++)
    {
      if (!g_main_context_iteration (NULL, FALSE))
        break;
    }
}

static void
pump_until_pending (guint timeout_ms)
{
  gint64 deadline = g_get_monotonic_time () + (gint64)timeout_ms * 1000;

  while (fake_pending_control () == NULL && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, TRUE);
}

static void
pump_until_cancelled (guint timeout_ms)
{
  gint64 deadline = g_get_monotonic_time () + (gint64)timeout_ms * 1000;

  while (fake.cancel_calls == 0 && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, TRUE);
}

static GUsbContext *
load_emulated_context (GUsbDevice **out_device)
{
  const gchar *json = "{"
                      "\"UsbDevices\":[{"
                      "\"PlatformId\":\"usb:0a:0b\","
                      "\"Created\":\"2026-10-19T00:00:00Z\","
                      "\"IdVendor\":4261,"
                      "\"IdProduct\":37377"
                      "}]"
                      "}";
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  GUsbContext *context;
  GUsbDevice *device;

  context = g_usb_context_new (&error);
  g_assert_no_error (error);
  g_assert_nonnull (context);

  parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);

  g_assert_true (
      g_usb_context_load (context, json_node_get_object (json_parser_get_root (parser)), &error));
  g_assert_no_error (error);

  device = g_usb_context_find_by_platform_id (context, "usb:0a:0b", &error);
  g_assert_no_error (error);
  g_assert_nonnull (device);

  *out_device = g_object_ref (device);

  return context;
}

static void
fake_device_from_identity (const FpiUsbIdentity *identity, guint8 address)
{
  guint count = identity->ports != NULL ? identity->ports->len : 0;

  g_assert_cmpuint (count, <=, 8);

  fake_device.descriptor.bLength = 18;
  fake_device.descriptor.bDescriptorType = 1;
  fake_device.descriptor.bcdUSB = 0x0200;
  fake_device.descriptor.bNumConfigurations = 1;
  fake_device.descriptor.idVendor = identity->vid;
  fake_device.descriptor.idProduct = identity->pid;
  fake_device.descriptor.bcdDevice = identity->release;

  fake_device.bus = identity->bus;
  fake_device.address = address;
  fake_device.port_count = count;
  if (count > 0)
    memcpy (fake_device.ports, identity->ports->data, count);
  fake_device.refs = 0;
}

static Fixture *
fixture_new (void)
{
  Fixture *fixture = g_new0 (Fixture, 1);
  g_autoptr (GError) error = NULL;
  FpiUsbIdentity identity = { 0 };

  fixture->usb_context = load_emulated_context (&fixture->usb);
  fixture->device = fpt_usb_device_new (fixture->usb);
  g_assert_nonnull (fixture->device);
  g_assert_true (FP_IS_DEVICE (fixture->device));

  fake_reset ();

  g_assert_true (fpi_usb_identity_from_device (fixture->usb, &identity));
  g_assert_nonnull (identity.ports);
  fake_device_from_identity (&identity, 7);
  fpi_usb_identity_clear (&identity);

  fixture->diagnostics = fpc_disum_diagnostics_new ();
  g_assert_nonnull (fixture->diagnostics);

  fixture->transport = fpc_disum_usb_transport_new (fixture->device, fixture->diagnostics, &error);
  g_assert_no_error (error);
  g_assert_nonnull (fixture->transport);

  return fixture;
}

static void
fixture_free (Fixture *fixture)
{
  if (fixture->transport != NULL)
    {
      fpc_disum_usb_transport_free (fixture->transport);
      fixture->transport = NULL;
      pump_main_context ();
      fake_reap_pending ();
    }

  fpc_disum_diagnostics_unref (fixture->diagnostics);

  if (fixture->device != NULL)
    g_object_unref (fixture->device);
  if (fixture->usb != NULL)
    g_object_unref (fixture->usb);
  if (fixture->usb_context != NULL)
    g_object_unref (fixture->usb_context);

  g_free (fixture);
}

static void
assert_fake_quiescent (void)
{
  g_assert_cmpint (fake_device.refs, ==, 0);
  g_assert_cmpint (fake.free_calls, ==, fake.alloc_calls);
  g_assert_cmpint (fake.exit_count, ==, fake.init_count);
  g_assert_true (fake.pending == NULL || g_queue_is_empty (fake.pending));
}

/* ------------------------------------------------------------------------- */
/* Completion capture                                                         */
/* ------------------------------------------------------------------------- */

typedef struct
{
  guint calls;
  FpcDisumTransportStatus status;
  gsize length;
  guint8 data[256];
} DataCapture;

static void
capture_data (FpcDisumTransport *transport, FpcDisumTransportStatus status, const guint8 *data,
    gsize length, gpointer user_data)

{
  DataCapture *capture = user_data;

  (void)transport;

  capture->calls++;
  capture->status = status;
  capture->length = length;
  if (data != NULL && length <= sizeof capture->data)
    memcpy (capture->data, data, length);
}

typedef struct
{
  guint calls;
  FpcDisumTransportStatus status;
  gsize actual_length;
} DoneCapture;

static void
capture_done (FpcDisumTransport *transport, FpcDisumTransportStatus status, gsize actual_length,
    gpointer user_data)
{
  DoneCapture *capture = user_data;

  (void)transport;

  capture->calls++;
  capture->status = status;
  capture->actual_length = actual_length;
}

typedef struct
{
  DoneCapture done;
  gboolean read_pending_at_done;
} ReopenCapture;

static void
capture_reopen_done (FpcDisumTransport *transport, FpcDisumTransportStatus status,
    gsize actual_length, gpointer user_data)
{
  ReopenCapture *capture = user_data;

  capture_done (transport, status, actual_length, &capture->done);
  /* The continuous reader must already be outstanding when the reopen
   * completes, so no initialisation frame can fall between the two. */
  capture->read_pending_at_done = fake_pending_read () != NULL;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void
test_factory_opens_and_claims (void)
{
  Fixture *fixture = fixture_new ();
  FpcDisumTransportIdentity identity;

  g_assert_cmpint (fake.claim_calls, ==, 1);
  g_assert_cmpint (fake.release_calls, ==, 0);
  g_assert_cmpint (fake.open_calls, ==, 1);
  g_assert_cmpint (fake.init_count, ==, 1);

  fixture->transport->identity (fixture->transport, &identity);
  g_assert_cmpint (identity.generation, ==, 0);
  g_assert_cmpuint (identity.handle_id, ==, 1);
  g_assert_cmpuint (identity.bus, ==, fake_device.bus);
  g_assert_cmpuint (identity.port_count, ==, fake_device.port_count);
  g_assert_cmpint (memcmp (identity.ports, fake_device.ports, fake_device.port_count), ==, 0);
  g_assert_cmpuint (identity.vid, ==, fake_device.descriptor.idVendor);
  g_assert_cmpuint (identity.pid, ==, fake_device.descriptor.idProduct);
  g_assert_cmpuint (identity.bcd_device, ==, fake_device.descriptor.bcdDevice);
  g_assert_cmpuint (identity.address, ==, fake_device.address);

  fixture_free (fixture);

  g_assert_cmpint (fake.close_calls, ==, 1);
  g_assert_cmpint (fake.release_calls, ==, 1);
  g_assert_cmpint (fake.exit_count, ==, 1);
  assert_fake_quiescent ();
}

static void
test_factory_rejects_identity_mismatch (void)
{
  Fixture *unused = NULL;
  g_autoptr (GError) error = NULL;
  FpiUsbIdentity identity = { 0 };

  unused = g_new0 (Fixture, 1);
  unused->usb_context = load_emulated_context (&unused->usb);
  unused->device = fpt_usb_device_new (unused->usb);

  fake_reset ();

  g_assert_true (fpi_usb_identity_from_device (unused->usb, &identity));
  fake_device_from_identity (&identity, 7);
  /* Same VID/PID/release, different bus. */
  fake_device.bus = (guint8)(identity.bus + 1);
  fpi_usb_identity_clear (&identity);

  g_assert_null (fpc_disum_usb_transport_new (unused->device, NULL, &error));
  g_assert_nonnull (error);
  g_assert_cmpint (fake.claim_calls, ==, 0);
  g_assert_cmpint (fake_device.refs, ==, 0);

  g_object_unref (unused->device);
  g_object_unref (unused->usb);
  g_object_unref (unused->usb_context);
  g_free (unused);
}

static void
test_factory_open_error_is_native (void)
{
  Fixture *unused;
  g_autoptr (GError) error = NULL;
  FpiUsbIdentity identity = { 0 };

  unused = g_new0 (Fixture, 1);
  unused->usb_context = load_emulated_context (&unused->usb);
  unused->device = fpt_usb_device_new (unused->usb);

  fake_reset ();
  fake.open_error = LIBUSB_ERROR_ACCESS;

  g_assert_true (fpi_usb_identity_from_device (unused->usb, &identity));
  fake_device_from_identity (&identity, 7);
  fpi_usb_identity_clear (&identity);

  g_assert_null (fpc_disum_usb_transport_new (unused->device, NULL, &error));
  g_assert_nonnull (error);
  g_assert_nonnull (strstr (error->message, "LIBUSB_ERROR_ACCESS"));
  g_assert_cmpint (fake.claim_calls, ==, 0);
  g_assert_cmpint (fake_device.refs, ==, 0);

  g_object_unref (unused->device);
  g_object_unref (unused->usb);
  g_object_unref (unused->usb_context);
  g_free (unused);
}

static void
test_factory_claim_failure_never_commits (void)
{
  Fixture *unused;
  g_autoptr (GError) error = NULL;
  FpiUsbIdentity identity = { 0 };

  unused = g_new0 (Fixture, 1);
  unused->usb_context = load_emulated_context (&unused->usb);
  unused->device = fpt_usb_device_new (unused->usb);

  fake_reset ();
  fake.claim_error = LIBUSB_ERROR_BUSY;

  g_assert_true (fpi_usb_identity_from_device (unused->usb, &identity));
  fake_device_from_identity (&identity, 7);
  fpi_usb_identity_clear (&identity);

  g_assert_null (fpc_disum_usb_transport_new (unused->device, NULL, &error));
  g_assert_nonnull (error);
  g_assert_nonnull (strstr (error->message, "claiming interface 0"));
  g_assert_false (fpi_device_usb_is_reenumeration_expected (unused->device));
  g_assert_cmpint (fake.claim_calls, ==, 1);
  g_assert_cmpint (fake.close_calls, ==, 1);
  g_assert_cmpint (fake_device.refs, ==, 0);

  g_object_unref (unused->device);
  g_object_unref (unused->usb);
  g_object_unref (unused->usb_context);
  g_free (unused);
}

static void
test_partial_timeout_then_ok (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture first = { 0 };
  DataCapture second = { 0 };
  struct libusb_transfer *transfer;
  static const guint8 part1[] = { 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00 };
  static const guint8 part2[] = { 0x0c, 0x00, 0x00, 0x00, 0x00 };

  fixture->transport->bulk_read (fixture->transport, capture_data, &first);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  memcpy (transfer->buffer, part1, sizeof part1);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, (int)sizeof part1);
  pump_main_context ();

  g_assert_cmpuint (first.calls, ==, 1);
  g_assert_cmpint (first.status, ==, FPC_DISUM_TRANSPORT_TIMEOUT);
  g_assert_cmpuint (first.length, ==, sizeof part1);
  g_assert_cmpint (memcmp (first.data, part1, sizeof part1), ==, 0);

  fixture->transport->bulk_read (fixture->transport, capture_data, &second);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  memcpy (transfer->buffer, part2, sizeof part2);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, (int)sizeof part2);
  pump_main_context ();

  g_assert_cmpuint (second.calls, ==, 1);
  g_assert_cmpint (second.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_cmpuint (second.length, ==, sizeof part2);
  g_assert_cmpint (memcmp (second.data, part2, sizeof part2), ==, 0);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_zlp_is_ok (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  struct libusb_transfer *transfer;

  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, 0);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_cmpuint (capture.length, ==, 0);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_cancel_completes_from_real_callback (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  struct libusb_transfer *transfer;

  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);

  fixture->transport->cancel (fixture->transport);
  g_assert_cmpint (fake.cancel_calls, ==, 1);
  pump_main_context ();
  /* The completion is deferred: cancel() must not deliver anything itself. */
  g_assert_cmpuint (capture.calls, ==, 0);

  /* The tail that was already transferred is still reported. */
  memset (transfer->buffer, 0xAB, 12);
  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 12);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);
  g_assert_cmpuint (capture.length, ==, 12);
  g_assert_cmpint (capture.data[0], ==, 0xAB);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_submit_failure_completes_once (void)
{
  Fixture *fixture = fixture_new ();
  DoneCapture capture = { 0 };
  static const guint8 command[4] = { 0x01, 0x02, 0x03, 0x04 };

  fake.submit_error = LIBUSB_ERROR_NO_DEVICE;

  fixture->transport->control_out (
      fixture->transport, 0x0b, 0x0000, command, sizeof command, capture_done, &capture);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE);
  g_assert_cmpuint (capture.actual_length, ==, 0);
  g_assert_cmpint (fake.submit_calls, ==, 2); /* the factory read URB + the failed control */
  g_assert_cmpint (fake.free_calls, ==, 1);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_short_control_out_reports_actual (void)
{
  Fixture *fixture = fixture_new ();
  DoneCapture capture = { 0 };
  static const guint8 command[4] = { 0x01, 0x02, 0x03, 0x04 };
  struct libusb_transfer *transfer;

  fixture->transport->control_out (
      fixture->transport, 0x0b, 0x0000, command, sizeof command, capture_done, &capture);
  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  g_assert_cmpuint (transfer->length, ==, LIBUSB_CONTROL_SETUP_SIZE + 4);

  /* The device acknowledges only two payload bytes. */
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, 2);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_cmpuint (capture.actual_length, ==, 2);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_pollfd_add_remove (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  struct libusb_transfer *transfer;

  g_assert_nonnull (fake.added_cb);
  g_assert_nonnull (fake.removed_cb);

  fake.removed_cb (fake.pipefd[0], fake.notifier_data);
  fake.added_cb (fake.pipefd[0], POLLIN, fake.notifier_data);

  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, 3);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpuint (capture.length, ==, 3);

  /* Delivery still works after another add/remove cycle. */
  fake.removed_cb (fake.pipefd[0], fake.notifier_data);
  fake.added_cb (fake.pipefd[0], POLLIN, fake.notifier_data);

  capture.calls = 0;
  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, 0);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_OK);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_free_waits_for_delayed_completion (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  struct libusb_transfer *transfer;

  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);

  fpc_disum_usb_transport_free (fixture->transport);
  fixture->transport = NULL;

  /* The kernel still owns the transfer: nothing may be closed or freed yet. */
  g_assert_cmpint (fake.cancel_calls, ==, 1);
  g_assert_cmpint (fake.close_calls, ==, 0);
  g_assert_cmpint (fake.release_calls, ==, 0);
  g_assert_cmpint (fake.free_calls, ==, 0);
  g_assert_cmpint (fake.exit_count, ==, 0);

  memset (transfer->buffer, 0x5A, 16);
  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 16);
  pump_main_context ();

  /* No callback after free, and exactly one release of everything. */
  g_assert_cmpuint (capture.calls, ==, 0);
  g_assert_cmpint (fake.free_calls, ==, 1);
  g_assert_cmpint (fake.close_calls, ==, 1);
  g_assert_cmpint (fake.release_calls, ==, 1);
  g_assert_cmpint (fake.exit_count, ==, 1);
  g_assert_cmpint (fake_device.refs, ==, 0);

  fpc_disum_diagnostics_unref (fixture->diagnostics);
  g_object_unref (fixture->device);
  g_object_unref (fixture->usb);
  g_object_unref (fixture->usb_context);
  g_free (fixture);

  assert_fake_quiescent ();
}

static void
test_reopen_keeps_live_handle (void)
{
  Fixture *fixture = fixture_new ();
  ReopenCapture capture = { 0 };
  struct libusb_transfer *transfer;
  FpcDisumTransportIdentity identity;
  static const guint8 descriptor[18]
      = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xA5, 0x10, 0x01, 0x92, 0x00, 0x00, 1, 2, 3, 1 };

  fixture->transport->reopen (fixture->transport, g_get_monotonic_time () + 2 * G_USEC_PER_SEC,
      capture_reopen_done, &capture);

  /* The reader is not pre-cancelled: it stays outstanding for the reap window
   * so the reset window keeps being drained. */
  g_assert_null (fake_pending_control ());
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  g_assert_cmpint (fake.cancel_calls, ==, 0);

  /* A terminal completion ends the reap without a host cancellation. */
  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 0);
  pump_main_context ();

  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  g_assert_cmpint (transfer->type, ==, LIBUSB_TRANSFER_TYPE_CONTROL);

  {
    guint8 payload[18];

    memcpy (payload, descriptor, sizeof payload);
    payload[8] = (guint8)(fake_device.descriptor.idVendor & 0xff);
    payload[9] = (guint8)(fake_device.descriptor.idVendor >> 8);
    payload[10] = (guint8)(fake_device.descriptor.idProduct & 0xff);
    payload[11] = (guint8)(fake_device.descriptor.idProduct >> 8);
    payload[12] = (guint8)(fake_device.descriptor.bcdDevice & 0xff);
    payload[13] = (guint8)(fake_device.descriptor.bcdDevice >> 8);
    fake_complete_control (transfer, LIBUSB_TRANSFER_COMPLETED, payload, sizeof payload);
  }
  pump_main_context ();

  g_assert_cmpuint (capture.done.calls, ==, 1);
  g_assert_cmpint (capture.done.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_true (capture.read_pending_at_done);
  g_assert_cmpint (fake.close_calls, ==, 0);
  g_assert_cmpint (fake.open_calls, ==, 1);

  fixture->transport->identity (fixture->transport, &identity);
  g_assert_cmpuint (identity.bus, ==, fake_device.bus);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_reopen_waits_for_new_handle (void)
{
  Fixture *fixture = fixture_new ();
  ReopenCapture capture = { 0 };
  struct libusb_transfer *transfer;
  FpcDisumTransportIdentity identity;
  guint8 payload[18] = { 0 };
  static const guint8 bad[18]
      = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x11, 0x22, 0x33, 0x44, 0, 0, 1, 2, 3, 1 };

  fixture->transport->reopen (fixture->transport, g_get_monotonic_time () + 3 * G_USEC_PER_SEC,
      capture_reopen_done, &capture);

  /* Reap the old read URB first, without a host cancellation. */
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  g_assert_cmpint (fake.cancel_calls, ==, 0);
  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 0);
  pump_main_context ();

  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);

  /* The old handle is dead: the probe fails with no device. */
  fake_complete (transfer, LIBUSB_TRANSFER_NO_DEVICE, 0);
  pump_main_context ();
  g_assert_cmpuint (capture.done.calls, ==, 0);
  g_assert_cmpint (fake.close_calls, ==, 1);

  /* The fresh list is taken 200 ms later; the candidate is opened, claimed and
   * probed before it replaces the old handle. */
  fake_device.address = 9;
  memcpy (payload, bad, sizeof payload);
  payload[8] = (guint8)(fake_device.descriptor.idVendor & 0xff);
  payload[9] = (guint8)(fake_device.descriptor.idVendor >> 8);
  payload[10] = (guint8)(fake_device.descriptor.idProduct & 0xff);
  payload[11] = (guint8)(fake_device.descriptor.idProduct >> 8);
  payload[12] = (guint8)(fake_device.descriptor.bcdDevice & 0xff);
  payload[13] = (guint8)(fake_device.descriptor.bcdDevice >> 8);

  pump_until_pending (2000);
  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  g_assert_cmpint (transfer->type, ==, LIBUSB_TRANSFER_TYPE_CONTROL);
  g_assert_cmpint (fake.open_calls, ==, 2);
  g_assert_cmpint (fake.claim_calls, ==, 2);

  fake_complete_control (transfer, LIBUSB_TRANSFER_COMPLETED, payload, sizeof payload);
  pump_main_context ();

  g_assert_cmpuint (capture.done.calls, ==, 1);
  g_assert_cmpint (capture.done.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_true (capture.read_pending_at_done);

  fixture->transport->identity (fixture->transport, &identity);
  g_assert_cmpuint (identity.handle_id, ==, 2);
  g_assert_cmpuint (identity.address, ==, 9);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_read_submit_failure_is_terminal (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  struct libusb_transfer *transfer;

  /* The reader re-arms after a short completion, but the next submit fails:
   * that must surface as a real error, never as an empty success. */
  fake.submit_error = LIBUSB_ERROR_NO_DEVICE;

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, 0);
  pump_main_context ();

  g_assert_null (fake_pending_read ());

  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE);
  g_assert_cmpuint (capture.length, ==, 0);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_cancel_during_reopen_reap (void)
{
  Fixture *fixture = fixture_new ();
  DoneCapture capture = { 0 };
  struct libusb_transfer *transfer;

  fixture->transport->reopen (
      fixture->transport, g_get_monotonic_time () + 2 * G_USEC_PER_SEC, capture_done, &capture);

  /* The reopen is waiting for the old read URB; it was not pre-cancelled. */
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  g_assert_cmpint (fake.cancel_calls, ==, 0);

  fixture->transport->cancel (fixture->transport);
  g_assert_cmpint (fake.cancel_calls, ==, 1);
  g_assert_cmpuint (capture.calls, ==, 0);

  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 0);
  pump_main_context ();

  /* A cancelled reopen completes exactly once and never starts a probe. */
  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);
  g_assert_null (fake_pending_control ());

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_read_starts_before_session (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };

  /* The backend owns the read URB from the factory moment; no session read or
   * control was issued, and nothing was delivered anywhere. */
  g_assert_nonnull (fake_pending_read ());
  g_assert_null (fake_pending_control ());
  g_assert_cmpuint (capture.calls, ==, 0);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_queued_frame_survives_reopen (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  ReopenCapture reopen = { 0 };
  struct libusb_transfer *transfer;
  static const guint8 init_result[12]
      = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00 };
  guint8 payload[18] = { 0 };

  /* A frame arrives before the session's first bulk_read and before CMD_INIT.
   * The backend must keep it. */
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  memcpy (transfer->buffer, init_result, sizeof init_result);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, (int)sizeof init_result);
  pump_main_context ();

  /* The reader immediately re-armed itself. */
  g_assert_nonnull (fake_pending_read ());

  /* The device resets: the outstanding read dies.  That alone must not fail
   * anything; the reader pauses and keeps the queued frame. */
  fake_complete (fake_pending_read (), LIBUSB_TRANSFER_NO_DEVICE, 0);
  pump_main_context ();
  g_assert_null (fake_pending_read ());

  fixture->transport->reopen (fixture->transport, g_get_monotonic_time () + 3 * G_USEC_PER_SEC,
      capture_reopen_done, &reopen);

  /* The read was already reaped, so the old-handle probe goes out at once. */
  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_NO_DEVICE, 0);
  pump_main_context ();

  fake_device.address = 9;
  payload[0] = 18;
  payload[1] = 1;
  payload[2] = 0x00;
  payload[3] = 0x02;
  payload[7] = 64;
  payload[8] = (guint8)(fake_device.descriptor.idVendor & 0xff);
  payload[9] = (guint8)(fake_device.descriptor.idVendor >> 8);
  payload[10] = (guint8)(fake_device.descriptor.idProduct & 0xff);
  payload[11] = (guint8)(fake_device.descriptor.idProduct >> 8);
  payload[12] = (guint8)(fake_device.descriptor.bcdDevice & 0xff);
  payload[13] = (guint8)(fake_device.descriptor.bcdDevice >> 8);
  payload[14] = 1;
  payload[15] = 2;
  payload[16] = 3;
  payload[17] = 1;

  pump_until_pending (2000);
  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  fake_complete_control (transfer, LIBUSB_TRANSFER_COMPLETED, payload, sizeof payload);
  pump_main_context ();

  g_assert_cmpuint (reopen.done.calls, ==, 1);
  g_assert_cmpint (reopen.done.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_true (reopen.read_pending_at_done);

  /* The first post-reopen session read receives the pre-reset frame. */
  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_cmpuint (capture.length, ==, sizeof init_result);
  g_assert_cmpint (memcmp (capture.data, init_result, sizeof init_result), ==, 0);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

/*
 * The reopen must not pre-cancel the old read.  The URB stays outstanding
 * across the reset, the 38 B initialisation frame is queued, and the natural
 * NO_DEVICE completion then releases the handle.
 */
static void
test_reopen_waits_for_natural_reset (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  ReopenCapture reopen = { 0 };
  struct libusb_transfer *transfer;
  guint8 init_result[38] = { 0 };
  guint8 payload[18] = { 0 };

  init_result[3] = 0x02; /* EVT_INIT_RESULT */
  init_result[7] = 38;
  memset (init_result + 8, 0x5A, sizeof init_result - 8);

  fixture->transport->reopen (fixture->transport, g_get_monotonic_time () + 5 * G_USEC_PER_SEC,
      capture_reopen_done, &reopen);

  /* Nothing was cancelled and the old read is still the outstanding URB. */
  g_assert_cmpint (fake.cancel_calls, ==, 0);
  g_assert_null (fake_pending_control ());
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);

  /* The initialisation frame arrives while the device is resetting.  It must
   * be kept even though the session read is disabled during reopen. */
  memcpy (transfer->buffer, init_result, sizeof init_result);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, (int)sizeof init_result);
  pump_main_context ();

  /* The reap keeps draining the old handle: no swap and no cancellation. */
  g_assert_cmpint (fake.cancel_calls, ==, 0);
  g_assert_null (fake_pending_control ());
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);

  /* The reset now ends the old read naturally. */
  fake_complete (transfer, LIBUSB_TRANSFER_NO_DEVICE, 0);
  pump_main_context ();

  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_NO_DEVICE, 0);
  pump_main_context ();

  fake_device.address = 9;
  payload[0] = 18;
  payload[1] = 1;
  payload[2] = 0x00;
  payload[3] = 0x02;
  payload[7] = 64;
  payload[8] = (guint8)(fake_device.descriptor.idVendor & 0xff);
  payload[9] = (guint8)(fake_device.descriptor.idVendor >> 8);
  payload[10] = (guint8)(fake_device.descriptor.idProduct & 0xff);
  payload[11] = (guint8)(fake_device.descriptor.idProduct >> 8);
  payload[12] = (guint8)(fake_device.descriptor.bcdDevice & 0xff);
  payload[13] = (guint8)(fake_device.descriptor.bcdDevice >> 8);
  payload[14] = 1;
  payload[15] = 2;
  payload[16] = 3;
  payload[17] = 1;

  pump_until_pending (2000);
  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  fake_complete_control (transfer, LIBUSB_TRANSFER_COMPLETED, payload, sizeof payload);
  pump_main_context ();

  g_assert_cmpuint (reopen.done.calls, ==, 1);
  g_assert_cmpint (reopen.done.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_true (reopen.read_pending_at_done);

  /* The first post-reopen read delivers the pre-reset frame and its status. */
  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_TIMEOUT);
  g_assert_cmpuint (capture.length, ==, sizeof init_result);
  g_assert_cmpint (memcmp (capture.data, init_result, sizeof init_result), ==, 0);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

/*
 * If the old read only ever times out, the bounded reap grace still converges:
 * the transport cancels and reaps that URB, then performs the reopen.
 */
static void
test_reopen_grace_expires_without_terminal (void)
{
  Fixture *fixture = fixture_new ();
  DoneCapture capture = { 0 };
  struct libusb_transfer *transfer;
  guint8 payload[18] = { 0 };
  guint i;

  fixture->transport->reopen (
      fixture->transport, g_get_monotonic_time () + 5 * G_USEC_PER_SEC, capture_done, &capture);

  /* Timeout cycles are not reaps: the old URB is re-armed each time. */
  for (i = 0; i < 2; i++)
    {
      transfer = fake_pending_read ();
      g_assert_nonnull (transfer);
      fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, 0);
      pump_main_context ();

      g_assert_cmpint (fake.cancel_calls, ==, 0);
      g_assert_null (fake_pending_control ());
      g_assert_nonnull (fake_pending_read ());
    }

  /* At the grace bound the transport cancels the old URB and waits for its
   * real completion before swapping the handle. */
  pump_until_cancelled (FPC_DISUM_USB_READ_REAP_GRACE_MS + 2000);
  g_assert_cmpint (fake.cancel_calls, ==, 1);
  g_assert_null (fake_pending_control ());

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 0);
  pump_main_context ();

  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_NO_DEVICE, 0);
  pump_main_context ();

  fake_device.address = 9;
  payload[0] = 18;
  payload[1] = 1;
  payload[2] = 0x00;
  payload[3] = 0x02;
  payload[7] = 64;
  payload[8] = (guint8)(fake_device.descriptor.idVendor & 0xff);
  payload[9] = (guint8)(fake_device.descriptor.idVendor >> 8);
  payload[10] = (guint8)(fake_device.descriptor.idProduct & 0xff);
  payload[11] = (guint8)(fake_device.descriptor.idProduct >> 8);
  payload[12] = (guint8)(fake_device.descriptor.bcdDevice & 0xff);
  payload[13] = (guint8)(fake_device.descriptor.bcdDevice >> 8);
  payload[14] = 1;
  payload[15] = 2;
  payload[16] = 3;
  payload[17] = 1;

  pump_until_pending (2000);
  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  fake_complete_control (transfer, LIBUSB_TRANSFER_COMPLETED, payload, sizeof payload);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_nonnull (fake_pending_read ());

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_timeout_loop_keeps_queued_data (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  struct libusb_transfer *transfer;
  static const guint8 partial[5] = { 0x01, 0x02, 0x03, 0x04, 0x05 };

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  memcpy (transfer->buffer, partial, sizeof partial);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, (int)sizeof partial);
  pump_main_context ();

  /* Two empty timeout cycles must not discard the queued partial data. */
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, 0);
  pump_main_context ();

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, 0);
  pump_main_context ();

  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_TIMEOUT);
  g_assert_cmpuint (capture.length, ==, sizeof partial);
  g_assert_cmpint (memcmp (capture.data, partial, sizeof partial), ==, 0);

  /* The queue is empty now: a session read waits for the next outcome. */
  capture.calls = 0;
  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  pump_main_context ();
  g_assert_cmpuint (capture.calls, ==, 0);

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, 0);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_TIMEOUT);
  g_assert_cmpuint (capture.length, ==, 0);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_control_concurrent_with_read (void)
{
  Fixture *fixture = fixture_new ();
  DoneCapture capture = { 0 };
  struct libusb_transfer *transfer;
  static const guint8 command[4] = { 0x01, 0x02, 0x03, 0x04 };

  /* The read URB from the factory is outstanding. */
  g_assert_nonnull (fake_pending_read ());
  g_assert_cmpint (fake.submit_calls, ==, 1);

  fixture->transport->control_out (
      fixture->transport, 0x0b, 0x0000, command, sizeof command, capture_done, &capture);

  /* Both transfers are in the kernel at the same time: the read does not own
   * the control slot. */
  g_assert_nonnull (fake_pending_control ());
  g_assert_nonnull (fake_pending_read ());
  g_assert_cmpint (fake.submit_calls, ==, 2);

  fake_complete (fake_pending_control (), LIBUSB_TRANSFER_COMPLETED, 2);
  pump_main_context ();

  g_assert_cmpuint (capture.calls, ==, 1);
  g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_OK);
  g_assert_cmpuint (capture.actual_length, ==, 2);

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, 3);
  pump_main_context ();

  /* The continuous reader re-armed after the completion. */
  g_assert_nonnull (fake_pending_read ());

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_cancel_control_and_waiting_read (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture data = { 0 };
  DoneCapture control = { 0 };
  struct libusb_transfer *transfer;
  static const guint8 command[4] = { 0x0b, 0x00, 0x00, 0x00 };

  fixture->transport->bulk_read (fixture->transport, capture_data, &data);
  fixture->transport->control_out (
      fixture->transport, 0x0b, 0x0000, command, sizeof command, capture_done, &control);
  g_assert_nonnull (fake_pending_read ());
  g_assert_nonnull (fake_pending_control ());

  fixture->transport->cancel (fixture->transport);

  /* The control and the read URB were both cancelled; cancel() itself still
   * delivers nothing. */
  g_assert_cmpint (fake.cancel_calls, ==, 2);
  pump_main_context ();
  g_assert_cmpuint (data.calls, ==, 0);
  g_assert_cmpuint (control.calls, ==, 0);

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  memset (transfer->buffer, 0xCD, 4);
  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 4);

  transfer = fake_pending_control ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_CANCELLED, 0);
  pump_main_context ();

  /* Exactly one completion per operation, with the read tail preserved. */
  g_assert_cmpuint (data.calls, ==, 1);
  g_assert_cmpint (data.status, ==, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);
  g_assert_cmpuint (data.length, ==, 4);
  g_assert_cmpint (data.data[0], ==, 0xCD);

  g_assert_cmpuint (control.calls, ==, 1);
  g_assert_cmpint (control.status, ==, FPC_DISUM_TRANSPORT_ERROR_CANCELLED);

  pump_main_context ();
  g_assert_cmpuint (data.calls, ==, 1);
  g_assert_cmpuint (control.calls, ==, 1);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

static void
test_read_queue_is_bounded (void)
{
  Fixture *fixture = fixture_new ();
  DataCapture capture = { 0 };
  struct libusb_transfer *transfer;
  const guint chunk = FPC_DISUM_USB_READ_QUEUE_MAX_BYTES / FPC_DISUM_USB_READ_QUEUE_MAX_CHUNKS;
  guint i;

  g_assert_cmpuint (chunk, ==, FPC_DISUM_USB_BULK_READ_SIZE);

  /* Produce more chunks than the queue can hold, with no session read. */
  for (i = 0; i < FPC_DISUM_USB_READ_QUEUE_MAX_CHUNKS + 8; i++)
    {
      transfer = fake_pending_read ();
      g_assert_nonnull (transfer);
      memset (transfer->buffer, (int)(i + 1), chunk);
      fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, (int)chunk);
      pump_main_context ();
      g_assert_nonnull (fake_pending_read ());
    }

  /* Only the bound is kept, oldest first; the excess was dropped, so exactly
   * the bounded number of chunks can be drained. */
  for (i = 0; i < FPC_DISUM_USB_READ_QUEUE_MAX_CHUNKS; i++)
    {
      capture.calls = 0;
      fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
      pump_main_context ();

      g_assert_cmpuint (capture.calls, ==, 1);
      g_assert_cmpint (capture.status, ==, FPC_DISUM_TRANSPORT_OK);
      g_assert_cmpuint (capture.length, ==, chunk);
      g_assert_cmpint (capture.data[0], ==, (gint)(i + 1));
    }

  /* The queue is empty: the next read waits instead of returning a dropped
   * chunk. */
  capture.calls = 0;
  fixture->transport->bulk_read (fixture->transport, capture_data, &capture);
  pump_main_context ();
  g_assert_cmpuint (capture.calls, ==, 0);

  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, 0);
  pump_main_context ();
  g_assert_cmpuint (capture.calls, ==, 1);

  fixture_free (fixture);
  assert_fake_quiescent ();
}

/* ------------------------------------------------------------------------- */
/* Smoke mode: real backend completion path + parser + diagnostic JSONL       */
/* ------------------------------------------------------------------------- */

static const gchar *const diagnostic_keys[] = {
  "run_id",
  "op_id",
  "kind",
  "phase",
  "t_us",
  "generation",
  "handle_id",
  "bus",
  "port_numbers",
  "address",
  "vid",
  "pid",
  "bcd_device",
  "direction",
  "bm_request_type",
  "request",
  "value",
  "index",
  "requested_length",
  "actual_length",
  "status",
  "native_status",
  "native_domain",
  "submitted",
  "event_code",
  "total_len",
  "third_field",
  "partial_length",
  "discarded_length",
  "primary_result",
  "cleanup_result",
};

typedef struct
{
  FILE *file;
  FpcDisumDiagnostics *diagnostics;
  gboolean invalid_key;
} JsonlSink;

typedef struct
{
  FpcDisumParser *parser;
  guint calls;
} SmokeRead;

typedef struct
{
  guint frames;
  guint32 code;
  guint32 total_len;
} ParseCapture;

static void
jsonl_add_value (JsonBuilder *builder, GVariant *value)
{
  const GVariantType *type = g_variant_get_type (value);

  if (g_variant_type_is_maybe (type))
    {
      if (g_variant_n_children (value) == 0)
        {
          json_builder_add_null_value (builder);
          return;
        }

      {
        GVariant *inner = g_variant_get_child_value (value, 0);

        jsonl_add_value (builder, inner);
        g_variant_unref (inner);
      }
      return;
    }

  if (g_variant_type_equal (type, G_VARIANT_TYPE_STRING))
    json_builder_add_string_value (builder, g_variant_get_string (value, NULL));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_BOOLEAN))
    json_builder_add_boolean_value (builder, g_variant_get_boolean (value));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_BYTE))
    json_builder_add_int_value (builder, g_variant_get_byte (value));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_UINT16))
    json_builder_add_int_value (builder, g_variant_get_uint16 (value));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_UINT32))
    json_builder_add_int_value (builder, g_variant_get_uint32 (value));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_UINT64))
    json_builder_add_int_value (builder, (gint64)g_variant_get_uint64 (value));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_INT32))
    json_builder_add_int_value (builder, g_variant_get_int32 (value));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_INT64))
    json_builder_add_int_value (builder, g_variant_get_int64 (value));
  else if (g_variant_type_equal (type, G_VARIANT_TYPE ("ay")))
    {
      gsize i;

      json_builder_begin_array (builder);
      for (i = 0; i < g_variant_n_children (value); i++)
        {
          GVariant *byte = g_variant_get_child_value (value, i);

          json_builder_add_int_value (builder, g_variant_get_byte (byte));
          g_variant_unref (byte);
        }
      json_builder_end_array (builder);
    }
  else
    json_builder_add_null_value (builder);
}

static gboolean
key_is_allowed (const gchar *key)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (diagnostic_keys); i++)
    {
      if (g_strcmp0 (diagnostic_keys[i], key) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
smoke_observer (const FpcDisumDiagnosticRecord *record, gpointer user_data)
{
  JsonlSink *sink = user_data;
  GVariant *variant;
  GVariantIter iter;
  const gchar *key;
  GVariant *value;
  JsonBuilder *builder;
  JsonNode *root;
  JsonGenerator *generator;
  gchar *line;

  variant = fpc_disum_diagnostics_to_variant (sink->diagnostics, record);
  g_assert_nonnull (variant);

  builder = json_builder_new ();
  json_builder_begin_object (builder);

  g_variant_iter_init (&iter, variant);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      if (!key_is_allowed (key))
        sink->invalid_key = TRUE;
      else
        {
          json_builder_set_member_name (builder, key);
          jsonl_add_value (builder, value);
        }
      g_variant_unref (value);
    }

  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  line = json_generator_to_data (generator, NULL);
  fprintf (sink->file, "%s\n", line);
  fflush (sink->file);

  g_free (line);
  g_object_unref (generator);
  json_node_free (root);
  g_object_unref (builder);
  g_variant_unref (variant);
}

static void
smoke_on_data (FpcDisumTransport *transport, FpcDisumTransportStatus status, const guint8 *data,
    gsize length, gpointer user_data)
{
  SmokeRead *read = user_data;

  (void)transport;

  read->calls++;

  if ((status == FPC_DISUM_TRANSPORT_OK || status == FPC_DISUM_TRANSPORT_TIMEOUT) && data != NULL
      && length > 0)
    {
      g_assert_true (fpc_disum_parser_feed (read->parser, data, length));
    }
}

static void
smoke_on_frame (const FpcDisumFrame *frame, gpointer user_data)
{
  ParseCapture *capture = user_data;

  capture->frames++;
  capture->code = frame->code;
  capture->total_len = frame->total_len;
}

static gboolean
verify_smoke_events (const gchar *path)
{
  gchar *contents = NULL;
  gsize length = 0;
  gchar **lines;
  guint i;
  guint submits = 0;
  guint completes = 0;
  gchar *run_id = NULL;
  guint64 op_ids[2] = { 0, 0 };
  gchar *statuses[2] = { NULL, NULL };
  gint64 actual[2] = { -1, -1 };
  gboolean ok = FALSE;

  if (!g_file_get_contents (path, &contents, &length, NULL))
    return FALSE;

  lines = g_strsplit (contents, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (JsonParser) parser = json_parser_new ();
      JsonObject *object;
      const gchar *kind;
      const gchar *current_run;

      if (lines[i][0] == '\0')
        continue;

      if (!json_parser_load_from_data (parser, lines[i], -1, &error))
        goto out;

      object = json_node_get_object (json_parser_get_root (parser));
      kind = json_object_get_string_member (object, "kind");
      current_run = json_object_get_string_member (object, "run_id");

      if (g_strcmp0 (kind, "submit") == 0)
        {
          submits++;
        }
      else if (g_strcmp0 (kind, "complete") == 0)
        {
          if (completes >= 2)
            goto out;

          if (run_id == NULL)
            run_id = g_strdup (current_run);
          else if (g_strcmp0 (run_id, current_run) != 0)
            goto out;

          op_ids[completes] = (guint64)json_object_get_int_member (object, "op_id");
          statuses[completes] = g_strdup (json_object_get_string_member (object, "status"));
          actual[completes] = json_object_get_int_member (object, "actual_length");
          completes++;
        }
    }

  /* The continuous reader re-arms after every completion, so the log may end
   * with one more submit whose URB teardown cancels.  Exactly the two observed
   * read outcomes must still be complete and distinct. */
  ok = submits >= 2 && completes == 2 && g_strcmp0 (statuses[0], "timeout") == 0 && actual[0] == 7
       && g_strcmp0 (statuses[1], "ok") == 0 && actual[1] == 5 && op_ids[0] != 0 && op_ids[1] != 0
       && op_ids[0] != op_ids[1];

out:
  g_free (statuses[0]);
  g_free (statuses[1]);
  g_free (run_id);
  g_strfreev (lines);
  g_free (contents);

  return ok;
}

static void
smoke_setup_fake (Fixture *fixture)
{
  FpiUsbIdentity identity = { 0 };

  fake_reset ();

  g_assert_true (fpi_usb_identity_from_device (fixture->usb, &identity));
  fake_device_from_identity (&identity, 7);
  fpi_usb_identity_clear (&identity);

  fixture->diagnostics = fpc_disum_diagnostics_new ();
}

static int
run_smoke (const gchar *directory)
{
  gchar *events_path = NULL;
  FILE *events_file = NULL;
  int fd = -1;
  Fixture fixture = { 0 };
  JsonlSink sink = { 0 };
  FpcDisumParser parser = { 0 };
  ParseCapture parse = { 0 };
  SmokeRead read = { 0 };
  g_autoptr (GError) error = NULL;
  struct libusb_transfer *transfer;
  static const guint8 event_bytes[12]
      = { 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00 };
  GStatBuf status;
  int result = 2;

  if (g_mkdir_with_parents (directory, 0700) != 0)
    {
      g_printerr ("cannot create smoke directory %s: %s\n", directory, g_strerror (errno));
      return 2;
    }
  if (g_chmod (directory, 0700) != 0)
    {
      g_printerr ("cannot restrict smoke directory %s\n", directory);
      return 2;
    }

  events_path = g_build_filename (directory, "events.jsonl", NULL);
  fd = g_open (events_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    {
      g_printerr ("cannot create %s: %s\n", events_path, g_strerror (errno));
      g_free (events_path);
      return 2;
    }
  if (fchmod (fd, 0600) != 0)
    {
      close (fd);
      g_free (events_path);
      return 2;
    }
  events_file = fdopen (fd, "w");
  if (events_file == NULL)
    {
      close (fd);
      g_free (events_path);
      return 2;
    }

  fixture.usb_context = load_emulated_context (&fixture.usb);
  fixture.device = fpt_usb_device_new (fixture.usb);
  g_assert_nonnull (fixture.device);

  smoke_setup_fake (&fixture);
  g_assert_nonnull (fixture.diagnostics);

  sink.file = events_file;
  sink.diagnostics = fixture.diagnostics;
  fpc_disum_diagnostics_set_observer (fixture.diagnostics, smoke_observer, &sink);

  fixture.transport = fpc_disum_usb_transport_new (fixture.device, fixture.diagnostics, &error);
  if (fixture.transport == NULL)
    {
      g_printerr ("creating the transport failed: %s\n",
          error != NULL ? error->message : "no error reported");
      goto out;
    }

  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_EVENT, smoke_on_frame, &parse);
  read.parser = &parser;

  fixture.transport->bulk_read (fixture.transport, smoke_on_data, &read);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  memcpy (transfer->buffer, event_bytes, 7);
  fake_complete (transfer, LIBUSB_TRANSFER_TIMED_OUT, 7);
  pump_main_context ();

  fixture.transport->bulk_read (fixture.transport, smoke_on_data, &read);
  transfer = fake_pending_read ();
  g_assert_nonnull (transfer);
  memcpy (transfer->buffer, event_bytes + 7, 5);
  fake_complete (transfer, LIBUSB_TRANSFER_COMPLETED, 5);
  pump_main_context ();

  if (sink.invalid_key || parse.frames != 1 || parse.code != 6 || parse.total_len != 12
      || fpc_disum_parser_pending (&parser) != 0)
    {
      g_printerr ("smoke check failed (frames=%u code=%u total_len=%u partial=%zu)\n", parse.frames,
          parse.code, parse.total_len, fpc_disum_parser_pending (&parser));
      goto out;
    }

  if (g_stat (directory, &status) != 0 || (status.st_mode & 0777) != 0700)
    {
      g_printerr ("smoke directory permissions are wrong\n");
      goto out;
    }
  if (g_stat (events_path, &status) != 0 || (status.st_mode & 0777) != 0600)
    {
      g_printerr ("smoke output permissions are wrong\n");
      goto out;
    }

  g_print ("frames=1 code=6 total_len=12 partial=0\n");
  result = 0;

out:
  if (fixture.transport != NULL)
    {
      fpc_disum_usb_transport_free (fixture.transport);
      fixture.transport = NULL;
      pump_main_context ();
      fake_reap_pending ();
    }

  fpc_disum_parser_clear (&parser);
  fpc_disum_diagnostics_set_observer (fixture.diagnostics, NULL, NULL);
  fpc_disum_diagnostics_unref (fixture.diagnostics);
  if (fixture.device != NULL)
    g_object_unref (fixture.device);
  if (fixture.usb != NULL)
    g_object_unref (fixture.usb);
  if (fixture.usb_context != NULL)
    g_object_unref (fixture.usb_context);

  if (events_file != NULL)
    {
      fclose (events_file);
      events_file = NULL;
    }

  if (result == 0 && !verify_smoke_events (events_path))
    {
      g_printerr ("smoke event log does not contain the expected completions\n");
      result = 2;
    }

  g_free (events_path);

  return result;
}

/* ------------------------------------------------------------------------- */
/* Main                                                                       */
/* ------------------------------------------------------------------------- */

int
main (int argc, char **argv)
{
  const gchar *smoke_directory = NULL;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (g_strcmp0 (argv[i], "--smoke-output-dir") == 0 && i + 1 < argc)
        smoke_directory = argv[++i];
    }

  if (smoke_directory != NULL)
    return run_smoke (smoke_directory);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/usb/factory-opens-and-claims", test_factory_opens_and_claims);
  g_test_add_func (
      "/usb/factory-rejects-identity-mismatch", test_factory_rejects_identity_mismatch);
  g_test_add_func ("/usb/factory-open-error-is-native", test_factory_open_error_is_native);
  g_test_add_func (
      "/usb/factory-claim-failure-never-commits", test_factory_claim_failure_never_commits);
  g_test_add_func ("/usb/partial-timeout-then-ok", test_partial_timeout_then_ok);
  g_test_add_func ("/usb/zlp-is-ok", test_zlp_is_ok);
  g_test_add_func (
      "/usb/cancel-completes-from-real-callback", test_cancel_completes_from_real_callback);
  g_test_add_func ("/usb/submit-failure-completes-once", test_submit_failure_completes_once);
  g_test_add_func ("/usb/short-control-out-reports-actual", test_short_control_out_reports_actual);
  g_test_add_func ("/usb/pollfd-add-remove", test_pollfd_add_remove);
  g_test_add_func (
      "/usb/free-waits-for-delayed-completion", test_free_waits_for_delayed_completion);
  g_test_add_func ("/usb/reopen-keeps-live-handle", test_reopen_keeps_live_handle);
  g_test_add_func ("/usb/reopen-waits-for-new-handle", test_reopen_waits_for_new_handle);
  g_test_add_func ("/usb/cancel-during-reopen-reap", test_cancel_during_reopen_reap);
  g_test_add_func ("/usb/read-submit-failure-is-terminal", test_read_submit_failure_is_terminal);
  g_test_add_func ("/usb/read-starts-before-session", test_read_starts_before_session);
  g_test_add_func ("/usb/queued-frame-survives-reopen", test_queued_frame_survives_reopen);
  g_test_add_func ("/usb/reopen-waits-for-natural-reset", test_reopen_waits_for_natural_reset);
  g_test_add_func (
      "/usb/reopen-grace-expires-without-terminal", test_reopen_grace_expires_without_terminal);
  g_test_add_func ("/usb/timeout-loop-keeps-queued-data", test_timeout_loop_keeps_queued_data);
  g_test_add_func ("/usb/control-concurrent-with-read", test_control_concurrent_with_read);
  g_test_add_func ("/usb/cancel-control-and-waiting-read", test_cancel_control_and_waiting_read);
  g_test_add_func ("/usb/read-queue-is-bounded", test_read_queue_is_bounded);

  return g_test_run ();
}
