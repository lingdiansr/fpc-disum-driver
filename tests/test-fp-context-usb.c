/*
 * Offline USB hotplug fixture for FpContext and the core re-enumeration
 * transaction.  It compiles the production fp-context.c and loads purely
 * artificial GUsb JSON; no physical device is ever enumerated.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <string.h>

#include <libfprint/fprint.h>
#include <json-glib/json-glib.h>

#include "fpi-context.h"
#include "fpi-device.h"
#include "fp-context-private.h"

#include "test-device-fake-usb.h"

GType fpt_usb_device_get_type (void);
#define FPT_TYPE_USB_DEVICE (fpt_usb_device_get_type ())

/* The test executable provides the only driver the context may see. */
GArray *
fpi_get_driver_types (void)
{
  GArray *drivers = g_array_new (TRUE, FALSE, sizeof (GType));
  GType driver = fpt_usb_device_get_type ();

  g_array_append_val (drivers, driver);
  return drivers;
}

#define DEFAULT_PLATFORM "usb:0a:0b"
#define SECOND_PLATFORM "usb:0a:0c"
#define DEFAULT_CREATED "2026-10-19T00:00:00Z"
#define SECOND_CREATED "2026-10-19T00:00:01Z"
#define FPT_VID 0x10a5
#define FPT_PID 0x9201
#define FPT_RELEASE 0x0250

typedef struct
{
  FpContext *context;
  GUsbContext *usb_ctx;
  gint added;
  gint removed;
} Fixture;

typedef struct
{
  gboolean done;
  gboolean ok;
  GError *error;
} OpenData;

typedef struct
{
  gboolean done;
  gboolean ok;
  GError *error;
} CloseData;

static void
drain_main_context (void)
{
  GMainContext *context = g_main_context_ref_thread_default ();

  while (g_main_context_iteration (context, FALSE))
    ;

  g_main_context_unref (context);
}

static gchar *
make_json (const gchar *platform_id, const gchar *created, guint vid, guint pid, guint release)
{
  return g_strdup_printf ("{\"UsbDevices\":[{\"PlatformId\":\"%s\",\"Created\":\"%s\","
                          "\"IdVendor\":%u,\"IdProduct\":%u,\"Device\":%u,\"USB\":512}]}",
      platform_id, created, vid, pid, release);
}

static gchar *
make_json_two (
    const gchar *platform1, const gchar *created1, const gchar *platform2, const gchar *created2)
{
  return g_strdup_printf ("{\"UsbDevices\":["
                          "{\"PlatformId\":\"%s\",\"Created\":\"%s\",\"IdVendor\":%u,"
                          "\"IdProduct\":%u,\"Device\":%u,\"USB\":512},"
                          "{\"PlatformId\":\"%s\",\"Created\":\"%s\",\"IdVendor\":%u,"
                          "\"IdProduct\":%u,\"Device\":%u,\"USB\":512}]}",
      platform1, created1, FPT_VID, FPT_PID, FPT_RELEASE, platform2, created2, FPT_VID, FPT_PID,
      FPT_RELEASE);
}

static void
fixture_load (Fixture *fixture, const gchar *json)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  JsonNode *root;

  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);

  root = json_parser_get_root (parser);
  g_assert_true (JSON_NODE_HOLDS_OBJECT (root));

  g_assert_true (g_usb_context_load (fixture->usb_ctx, json_node_get_object (root), &error));
  g_assert_no_error (error);
}

static void
context_device_added_cb (FpContext *context, FpDevice *device, gpointer user_data)
{
  Fixture *fixture = user_data;

  g_atomic_int_inc (&fixture->added);
}

static void
context_device_removed_cb (FpContext *context, FpDevice *device, gpointer user_data)
{
  Fixture *fixture = user_data;

  g_atomic_int_inc (&fixture->removed);
}

static void
device_removed_signal_cb (FpDevice *device, gpointer user_data)
{
  g_atomic_int_inc ((gint *)user_data);
}

static void
fixture_init (Fixture *fixture, const gchar *json)
{
  memset (fixture, 0, sizeof (*fixture));

  fixture->context = fp_context_new ();
  g_assert_true (FP_CONTEXT (fixture->context));

  fixture->usb_ctx = fpi_context_get_usb_context (fixture->context);
  g_assert_nonnull (fixture->usb_ctx);

  g_signal_connect (
      fixture->context, "device-added", G_CALLBACK (context_device_added_cb), fixture);
  g_signal_connect (
      fixture->context, "device-removed", G_CALLBACK (context_device_removed_cb), fixture);

  fixture_load (fixture, json);
  drain_main_context ();

  /* Mark the context as enumerated while its device list is drained. */
  g_assert_nonnull (fp_context_get_devices (fixture->context));
}

static void
fixture_clear (Fixture *fixture)
{
  g_clear_object (&fixture->context);
  fixture->usb_ctx = NULL;
}

static GUsbDevice *
fixture_find (Fixture *fixture, const gchar *platform_id)
{
  g_autoptr (GError) error = NULL;
  GUsbDevice *device;

  device = g_usb_context_find_by_platform_id (fixture->usb_ctx, platform_id, &error);
  g_assert_no_error (error);
  g_assert_nonnull (device);

  return device; /* owned */
}

static guint
fixture_n_devices (Fixture *fixture)
{
  return fp_context_get_devices (fixture->context)->len;
}

static FpDevice *
fixture_device (Fixture *fixture, guint index)
{
  GPtrArray *devices = fp_context_get_devices (fixture->context);

  g_assert_cmpuint (index, <, devices->len);
  return g_ptr_array_index (devices, index);
}

static void
open_done_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  OpenData *data = user_data;

  data->done = TRUE;
  data->ok = fp_device_open_finish (FP_DEVICE (source_object), res, &data->error);
}

static void
close_done_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  CloseData *data = user_data;

  data->done = TRUE;
  data->ok = fp_device_close_finish (FP_DEVICE (source_object), res, &data->error);
}

static void
fixture_open_deferred (Fixture *fixture, FpDevice *device, OpenData *data)
{
  g_object_set (device, "defer-open", TRUE, NULL);
  fp_device_open (device, NULL, open_done_cb, data);
  drain_main_context ();
  g_assert_false (data->done);
}

static void
fixture_complete_open (FpDevice *device, OpenData *data)
{
  fpi_device_open_complete (device, NULL);
  drain_main_context ();
  g_assert_true (data->done);
  g_assert_no_error (data->error);
  g_assert_true (data->ok);
  g_assert_true (fp_device_is_open (device));
}

static void
fixture_close (FpDevice *device, CloseData *data)
{
  g_assert_true (fp_device_is_open (device));
  fp_device_close (device, NULL, close_done_cb, data);
  drain_main_context ();
  g_assert_true (data->done);
  g_assert_no_error (data->error);
  g_assert_true (data->ok);
  g_assert_false (fp_device_is_open (device));
}

static void
weak_finalized_cb (gpointer data, GObject *where_the_object_was)
{
  gboolean *finalized = data;

  *finalized = TRUE;
}

/* --- identity parsing ---------------------------------------------------- */

static void
test_identity_parse (void)
{
  guint8 bus;
  GArray *ports = NULL;

  g_assert_true (fpi_usb_identity_from_platform_id ("usb:0a:0b", &bus, &ports));
  g_assert_cmpuint (bus, ==, 0x0a);
  g_assert_nonnull (ports);
  g_assert_cmpuint (ports->len, ==, 1);
  g_assert_cmpuint (g_array_index (ports, guint8, 0), ==, 0x0b);
  g_clear_pointer (&ports, g_array_unref);

  /* "10" is hexadecimal, not decimal. */
  g_assert_true (fpi_usb_identity_from_platform_id ("usb:10:02", &bus, &ports));
  g_assert_cmpuint (bus, ==, 0x10);
  g_assert_cmpuint (ports->len, ==, 1);
  g_assert_cmpuint (g_array_index (ports, guint8, 0), ==, 0x02);
  g_clear_pointer (&ports, g_array_unref);

  g_assert_true (fpi_usb_identity_from_platform_id ("usb:00:01:02:03", &bus, &ports));
  g_assert_cmpuint (bus, ==, 0x00);
  g_assert_cmpuint (ports->len, ==, 3);
  g_assert_cmpuint (g_array_index (ports, guint8, 0), ==, 0x01);
  g_assert_cmpuint (g_array_index (ports, guint8, 1), ==, 0x02);
  g_assert_cmpuint (g_array_index (ports, guint8, 2), ==, 0x03);
  g_clear_pointer (&ports, g_array_unref);

  /* GUsb's Linux path carries the root-hub port first; it is not part of the
   * device identity that libusb reports. */
  g_assert_true (fpi_usb_identity_from_platform_id ("usb:03:00:07", &bus, &ports));
  g_assert_cmpuint (bus, ==, 0x03);
  g_assert_cmpuint (ports->len, ==, 1);
  g_assert_cmpuint (g_array_index (ports, guint8, 0), ==, 0x07);
  g_clear_pointer (&ports, g_array_unref);

  /* Bad prefix, empty chain, non-hex, overflow/truncation. */
  g_assert_false (fpi_usb_identity_from_platform_id (NULL, &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:0a", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:0a:", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:0a::0b", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:0a:0g", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:0a:0b:0", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:0a:100", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("usb:100:0b", &bus, &ports));
  g_assert_null (ports);
  g_assert_false (fpi_usb_identity_from_platform_id ("pci:0a:0b", &bus, &ports));
  g_assert_null (ports);
}

static void
test_identity_device (void)
{
  Fixture fixture;
  FpiUsbIdentity identity = { 0 };
  FpiUsbIdentity second = { 0 };
  GUsbContext *other_ctx = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  GUsbDevice *usb;
  GUsbDevice *other;
  gchar *json;
  guint8 ports[] = { 0x0b };
  guint8 other_ports[] = { 0x0c };

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  g_assert_true (fpi_usb_identity_from_device (usb, &identity));
  g_assert_cmpstr (identity.platform_id, ==, DEFAULT_PLATFORM);
  g_assert_cmpuint (identity.bus, ==, 0x0a);
  g_assert_cmpuint (identity.vid, ==, FPT_VID);
  g_assert_cmpuint (identity.pid, ==, FPT_PID);
  g_assert_cmpuint (identity.release, ==, FPT_RELEASE);
  g_assert_cmpuint (identity.ports->len, ==, 1);
  g_assert_cmpuint (g_array_index (identity.ports, guint8, 0), ==, 0x0b);

  g_assert_true (fpi_usb_identity_from_device (usb, &second));
  g_assert_true (fpi_usb_identity_equal (&identity, &second));
  fpi_usb_identity_clear (&second);

  g_assert_true (fpi_usb_identity_matches_bus_ports (
      &identity, 0x0a, ports, 1, FPT_VID, FPT_PID, FPT_RELEASE));
  g_assert_false (fpi_usb_identity_matches_bus_ports (
      &identity, 0x0b, ports, 1, FPT_VID, FPT_PID, FPT_RELEASE));
  g_assert_false (fpi_usb_identity_matches_bus_ports (
      &identity, 0x0a, ports, 1, FPT_VID, FPT_PID, FPT_RELEASE + 1));
  g_assert_false (fpi_usb_identity_matches_bus_ports (
      &identity, 0x0a, ports, 1, FPT_VID + 1, FPT_PID, FPT_RELEASE));
  g_assert_false (fpi_usb_identity_matches_bus_ports (
      &identity, 0x0a, ports, 1, FPT_VID, FPT_PID + 1, FPT_RELEASE));
  g_assert_false (fpi_usb_identity_matches_bus_ports (
      &identity, 0x0a, ports, 0, FPT_VID, FPT_PID, FPT_RELEASE));
  g_assert_false (fpi_usb_identity_matches_bus_ports (
      &identity, 0x0a, other_ports, 1, FPT_VID, FPT_PID, FPT_RELEASE));

  /* A different complete chain with otherwise identical descriptors is a
   * different identity. */
  other_ctx = g_usb_context_new (&error);
  g_assert_no_error (error);
  g_assert_nonnull (other_ctx);

  json = make_json (SECOND_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE);
  parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  g_assert_true (
      g_usb_context_load (other_ctx, json_node_get_object (json_parser_get_root (parser)), &error));
  g_assert_no_error (error);
  g_free (json);

  other = g_usb_context_find_by_platform_id (other_ctx, SECOND_PLATFORM, &error);
  g_assert_no_error (error);
  g_assert_nonnull (other);

  g_assert_true (fpi_usb_identity_from_device (other, &second));
  g_assert_false (fpi_usb_identity_equal (&identity, &second));

  fpi_usb_identity_clear (&identity);
  fpi_usb_identity_clear (&second);
  g_object_unref (other);
  g_clear_object (&other_ctx);
  g_object_unref (usb);
  fixture_clear (&fixture);
}

/* --- plain hotplug ------------------------------------------------------- */

static void
test_add_remove (void)
{
  Fixture fixture;
  FpDevice *device;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  device = fixture_device (&fixture, 0);
  g_assert_cmpstr (fp_device_get_driver (device), ==, "fake_usb");

  json = g_strdup ("{\"UsbDevices\":[]}");
  fixture_load (&fixture, json);
  g_free (json);
  drain_main_context ();

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 0);
  g_assert_cmpuint (fixture.removed, ==, 1);

  fixture_clear (&fixture);
}

/* --- explicit transaction: same object ---------------------------------- */

static void
test_same_object_commit (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *usb;
  GUsbDevice *candidate;
  FpDevice *device;
  OpenData open_data = { 0 };
  CloseData close_data = { 0 };
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_is_reenumeration_expected (device));

  /* A second begin is refused while the transaction is active. */
  g_assert_false (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  /* A repeated add of the same object is not a second device or a new
   * generation. */
  fpi_device_usb_offer_replacement (device, usb);
  g_signal_emit_by_name (fixture.usb_ctx, "device-added", usb);
  drain_main_context ();

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  g_assert_cmpint (fixture.added, ==, 1);

  candidate = fpi_device_usb_ref_reenumeration_candidate (device);
  g_assert_true (candidate == usb);
  g_object_unref (candidate);

  /* No departure: commit must not close or reopen the core object. */
  g_assert_true (fpi_device_usb_commit_reenumeration (device, usb, &error));
  g_assert_no_error (error);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_true (fpi_device_get_usb_device (device) == usb);
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);

  fixture_complete_open (device, &open_data);

  fixture_close (device, &close_data);

  /* A second, independent open/close cycle on the same FpDevice. */
  memset (&open_data, 0, sizeof (open_data));
  memset (&close_data, 0, sizeof (close_data));
  g_object_set (device, "defer-open", FALSE, NULL);
  fp_device_open (device, NULL, open_done_cb, &open_data);
  drain_main_context ();
  g_assert_true (open_data.done);
  g_assert_no_error (open_data.error);
  g_assert_true (open_data.ok);
  g_assert_true (fp_device_is_open (device));
  fixture_close (device, &close_data);

  g_object_unref (usb);
  fixture_clear (&fixture);
}

static void
test_no_departure_commit_keeps_pointer (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *usb;
  FpDevice *device;
  OpenData open_data = { 0 };
  CloseData close_data = { 0 };
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  /* No offer and no departure: the current object is the candidate. */
  g_assert_true (fpi_device_usb_commit_reenumeration (device, usb, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_get_usb_device (device) == usb);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));

  fixture_complete_open (device, &open_data);
  fixture_close (device, &close_data);

  g_object_unref (usb);
  fixture_clear (&fixture);
}

/* --- failure paths ------------------------------------------------------- */

static void
test_departure_without_replacement (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *usb;
  GUsbDevice *candidate;
  FpDevice *device;
  OpenData open_data = { 0 };
  gint removed_signals = 0;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  g_signal_connect (device, "removed", G_CALLBACK (device_removed_signal_cb), &removed_signals);
  usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  /* The current object really leaves the bus while expected. */
  fixture_load (&fixture, "{\"UsbDevices\":[]}");
  drain_main_context ();

  g_assert_true (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);

  candidate = fpi_device_usb_ref_reenumeration_candidate (device);
  g_assert_null (candidate);

  /* No new object exists, so claiming the old one must fail. */
  g_assert_false (fpi_device_usb_commit_reenumeration (device, usb, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_true (fpi_device_get_usb_device (device) == usb);

  /* The failure path already aborted; abort stays idempotent. */
  fpi_device_usb_abort_reenumeration (device);
  fpi_device_usb_abort_reenumeration (device);
  drain_main_context ();
  g_assert_cmpint (removed_signals, ==, 0);

  /* Finish the open task so the deferred removal is actually reported. */
  fpi_device_open_complete (
      device, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled by test"));
  drain_main_context ();
  g_clear_error (&open_data.error);

  g_assert_cmpint (removed_signals, ==, 1);
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 0);

  g_object_unref (usb);
  fixture_clear (&fixture);
}

static void
test_deadline_expired (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *usb;
  FpDevice *device;
  OpenData open_data = { 0 };
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (device, g_get_monotonic_time () - 1, &error));
  g_assert_no_error (error);

  g_assert_false (fpi_device_usb_commit_reenumeration (device, usb, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));

  fpi_device_open_complete (
      device, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled by test"));
  drain_main_context ();
  g_clear_error (&open_data.error);

  /* No departure was ever seen: no fabricated removal. */
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  g_assert_cmpint (fixture.removed, ==, 0);

  g_object_unref (usb);
  fixture_clear (&fixture);
}

static void
test_wrong_candidate_rejected (void)
{
  Fixture fixture;
  GUsbContext *other_ctx = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  GUsbDevice *other;
  GUsbDevice *usb;
  FpDevice *device;
  OpenData open_data = { 0 };
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  other_ctx = g_usb_context_new (&error);
  g_assert_no_error (error);
  json = make_json (SECOND_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE);
  parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  g_assert_true (
      g_usb_context_load (other_ctx, json_node_get_object (json_parser_get_root (parser)), &error));
  g_assert_no_error (error);
  g_free (json);

  other = g_usb_context_find_by_platform_id (other_ctx, SECOND_PLATFORM, &error);
  g_assert_no_error (error);
  g_assert_nonnull (other);

  g_assert_false (fpi_device_usb_commit_reenumeration (device, other, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_true (fpi_device_get_usb_device (device) == usb);

  fpi_device_open_complete (
      device, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled by test"));
  drain_main_context ();
  g_clear_error (&open_data.error);
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);

  g_clear_object (&other_ctx);
  g_object_unref (other);
  g_object_unref (usb);
  fixture_clear (&fixture);
}

static void
test_cancelled_open_closes_and_aborts (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *usb;
  FpDevice *device;
  OpenData open_data = { 0 };
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  fpi_device_open_complete (
      device, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled by test"));
  drain_main_context ();

  g_assert_true (open_data.done);
  g_assert_false (open_data.ok);
  g_assert_error (open_data.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false (fp_device_is_open (device));
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  g_assert_cmpint (fixture.removed, ==, 0);

  g_clear_error (&open_data.error);
  g_object_unref (usb);
  fixture_clear (&fixture);
}

/* --- replacement commit -------------------------------------------------- */

static void
test_replacement_commit (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *old_usb;
  GUsbDevice *candidate;
  GUsbDevice *referenced;
  FpDevice *device;
  OpenData open_data = { 0 };
  CloseData close_data = { 0 };
  gboolean old_finalized = FALSE;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  old_usb = fixture_find (&fixture, DEFAULT_PLATFORM);
  g_object_weak_ref (G_OBJECT (old_usb), weak_finalized_cb, &old_finalized);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  /* Same PlatformId, different Created => remove plus new instance. */
  json = make_json (DEFAULT_PLATFORM, SECOND_CREATED, FPT_VID, FPT_PID, FPT_RELEASE);
  fixture_load (&fixture, json);
  g_free (json);

  candidate = fixture_find (&fixture, DEFAULT_PLATFORM);
  g_assert_true (candidate != old_usb);
  g_assert_true (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  g_assert_true (fixture_device (&fixture, 0) == device);
  g_assert_true (fpi_device_get_usb_device (device) == old_usb);
  g_assert_cmpint (fixture.removed, ==, 0);

  referenced = fpi_device_usb_ref_reenumeration_candidate (device);
  g_assert_true (referenced == candidate);
  g_object_unref (referenced);

  g_assert_true (fpi_device_usb_commit_reenumeration (device, candidate, &error));
  g_assert_no_error (error);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_true (fpi_device_get_usb_device (device) == candidate);
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  g_assert_true (fixture_device (&fixture, 0) == device);

  g_object_unref (old_usb);
  g_assert_true (old_finalized);

  fixture_complete_open (device, &open_data);
  fixture_close (device, &close_data);

  /* The next OPEN operates on the replacement core handle. */
  memset (&open_data, 0, sizeof (open_data));
  memset (&close_data, 0, sizeof (close_data));
  g_object_set (device, "defer-open", FALSE, NULL);
  fp_device_open (device, NULL, open_done_cb, &open_data);
  drain_main_context ();
  g_assert_true (open_data.done);
  g_assert_no_error (open_data.error);
  g_assert_true (fp_device_is_open (device));
  g_assert_true (fpi_device_get_usb_device (device) == candidate);
  fixture_close (device, &close_data);

  /* Removal of the current generation is a real removal. */
  fixture_load (&fixture, "{\"UsbDevices\":[]}");
  drain_main_context ();
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 0);

  g_object_unref (candidate);
  fixture_clear (&fixture);
}

static void
test_late_retired_removal_ignored (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *old_usb;
  GUsbDevice *candidate;
  FpDevice *device;
  OpenData open_data = { 0 };
  CloseData close_data = { 0 };
  gboolean removed = FALSE;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  old_usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  json = make_json (DEFAULT_PLATFORM, SECOND_CREATED, FPT_VID, FPT_PID, FPT_RELEASE);
  fixture_load (&fixture, json);
  g_free (json);

  candidate = fixture_find (&fixture, DEFAULT_PLATFORM);
  g_assert_true (fpi_device_usb_commit_reenumeration (device, candidate, &error));
  g_assert_no_error (error);

  /* A delayed removal of the retired object must not kill the new binding. */
  g_signal_emit_by_name (fixture.usb_ctx, "device-removed", old_usb);
  drain_main_context ();

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  g_assert_true (fpi_device_get_usb_device (device) == candidate);
  g_object_get (device, "removed", &removed, NULL);
  g_assert_false (removed);
  g_assert_cmpint (fixture.removed, ==, 0);

  fixture_complete_open (device, &open_data);
  fixture_close (device, &close_data);

  g_object_unref (candidate);
  g_object_unref (old_usb);
  fixture_clear (&fixture);
}

/* --- queued candidates --------------------------------------------------- */

static void
test_candidate_before_idle_deletion (void)
{
  Fixture fixture;
  FpDevice *device;
  FpDevice *replacement;
  GUsbDevice *old_usb;
  GUsbDevice *candidate;
  gboolean removed = FALSE;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  old_usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  g_assert_cmpint (fixture.added, ==, 1);

  /* A new independent instance while the old entry is still live and not
   * expected: retire the old one and queue the candidate. */
  json = make_json (DEFAULT_PLATFORM, SECOND_CREATED, FPT_VID, FPT_PID, FPT_RELEASE);
  fixture_load (&fixture, json);
  g_free (json);

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  g_assert_true (fixture_device (&fixture, 0) == device);
  g_assert_true (fpi_device_get_usb_device (device) == old_usb);
  g_object_get (device, "removed", &removed, NULL);
  g_assert_true (removed);
  g_assert_cmpint (fixture.added, ==, 1);

  candidate = fixture_find (&fixture, DEFAULT_PLATFORM);

  drain_main_context ();

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);
  replacement = fixture_device (&fixture, 0);
  g_assert_true (replacement != device);
  g_assert_true (fpi_device_get_usb_device (replacement) == candidate);
  g_assert_cmpint (fixture.added, ==, 2);

  fixture_load (&fixture, "{\"UsbDevices\":[]}");
  drain_main_context ();
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 0);

  g_object_unref (candidate);
  g_object_unref (old_usb);
  fixture_clear (&fixture);
}

static void
test_queued_candidate_departs (void)
{
  Fixture fixture;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  g_assert_cmpint (fixture.added, ==, 1);

  json = make_json (DEFAULT_PLATFORM, SECOND_CREATED, FPT_VID, FPT_PID, FPT_RELEASE);
  fixture_load (&fixture, json);
  g_free (json);

  /* The candidate leaves before the old entry was deleted. */
  fixture_load (&fixture, "{\"UsbDevices\":[]}");

  drain_main_context ();

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 0);
  /* The departed candidate was never published. */
  g_assert_cmpint (fixture.added, ==, 1);

  fixture_clear (&fixture);
}

static void
test_different_identity_keeps_discovery (void)
{
  Fixture fixture;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 1);

  json = make_json_two (DEFAULT_PLATFORM, DEFAULT_CREATED, SECOND_PLATFORM, SECOND_CREATED);
  fixture_load (&fixture, json);
  g_free (json);
  drain_main_context ();

  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 2);
  g_assert_cmpint (fixture.added, ==, 2);

  fixture_load (&fixture, "{\"UsbDevices\":[]}");
  drain_main_context ();
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 0);

  fixture_clear (&fixture);
}

static void
test_offer_released_on_abort (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  GUsbDevice *old_usb;
  GUsbDevice *candidate;
  GUsbDevice *referenced;
  FpDevice *device;
  OpenData open_data = { 0 };
  gboolean candidate_finalized = FALSE;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  device = fixture_device (&fixture, 0);
  old_usb = fixture_find (&fixture, DEFAULT_PLATFORM);

  fixture_open_deferred (&fixture, device, &open_data);

  g_assert_true (fpi_device_usb_begin_reenumeration (device, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_device_usb_allow_reenumeration (
      device, g_get_monotonic_time () + 20 * G_USEC_PER_SEC, &error));
  g_assert_no_error (error);

  json = make_json (DEFAULT_PLATFORM, SECOND_CREATED, FPT_VID, FPT_PID, FPT_RELEASE);
  fixture_load (&fixture, json);
  g_free (json);

  candidate = fixture_find (&fixture, DEFAULT_PLATFORM);
  g_object_weak_ref (G_OBJECT (candidate), weak_finalized_cb, &candidate_finalized);
  g_object_unref (candidate);

  referenced = fpi_device_usb_ref_reenumeration_candidate (device);
  g_assert_nonnull (referenced);
  g_object_unref (referenced);

  fpi_device_usb_abort_reenumeration (device);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (device));
  g_assert_null (fpi_device_usb_ref_reenumeration_candidate (device));
  g_assert_false (candidate_finalized); /* still held by the GUsb context */

  /* Dropping the GUsb context's reference must release the offered object. */
  fixture_load (&fixture, "{\"UsbDevices\":[]}");
  drain_main_context ();
  g_assert_true (candidate_finalized);

  fpi_device_open_complete (
      device, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled by test"));
  drain_main_context ();
  g_clear_error (&open_data.error);
  g_assert_cmpuint (fixture_n_devices (&fixture), ==, 0);

  g_object_unref (old_usb);
  fixture_clear (&fixture);
}

static void
test_begin_requires_open (void)
{
  Fixture fixture;
  g_autoptr (GError) error = NULL;
  gchar *json;

  fixture_init (&fixture,
      (json = make_json (DEFAULT_PLATFORM, DEFAULT_CREATED, FPT_VID, FPT_PID, FPT_RELEASE)));
  g_free (json);

  /* No OPEN action is running. */
  g_assert_false (fpi_device_usb_begin_reenumeration (fixture_device (&fixture, 0), &error));
  g_assert_nonnull (error);
  g_assert_false (fpi_device_usb_is_reenumeration_expected (fixture_device (&fixture, 0)));

  fixture_clear (&fixture);
}

int
main (int argc, char **argv)
{
  /* Only the artificial USB class may be discovered. */
  g_setenv ("FP_DRIVERS_ALLOWLIST", "fake_usb", TRUE);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/context-usb/identity-parse", test_identity_parse);
  g_test_add_func ("/context-usb/identity-device", test_identity_device);
  g_test_add_func ("/context-usb/add-remove", test_add_remove);
  g_test_add_func ("/context-usb/same-object-commit", test_same_object_commit);
  g_test_add_func (
      "/context-usb/no-departure-keeps-pointer", test_no_departure_commit_keeps_pointer);
  g_test_add_func (
      "/context-usb/departure-without-replacement", test_departure_without_replacement);
  g_test_add_func ("/context-usb/deadline-expired", test_deadline_expired);
  g_test_add_func ("/context-usb/wrong-candidate", test_wrong_candidate_rejected);
  g_test_add_func ("/context-usb/cancelled-open", test_cancelled_open_closes_and_aborts);
  g_test_add_func ("/context-usb/replacement-commit", test_replacement_commit);
  g_test_add_func ("/context-usb/late-retired-removal", test_late_retired_removal_ignored);
  g_test_add_func (
      "/context-usb/candidate-before-idle-deletion", test_candidate_before_idle_deletion);
  g_test_add_func ("/context-usb/queued-candidate-departs", test_queued_candidate_departs);
  g_test_add_func ("/context-usb/different-identity", test_different_identity_keeps_discovery);
  g_test_add_func ("/context-usb/offer-released-on-abort", test_offer_released_on_abort);
  g_test_add_func ("/context-usb/begin-requires-open", test_begin_requires_open);

  return g_test_run ();
}
