/*
 * FPC Disum (10a5:9201) de-sensitised completion records.
 *
 * Reference: docs/CLEANROOM_SPEC.md appendix C.4.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_diagnostics.h"

struct _FpcDisumDiagnostics
{
  gatomicrefcount ref_count;
  gchar *run_id;
  guint64 next_op_id;
  FpcDisumDiagnosticPhase phase;
  FpcDisumDiagnosticFunc observer;
  gpointer observer_data;
};

FpcDisumDiagnostics *
fpc_disum_diagnostics_new (void)
{
  FpcDisumDiagnostics *self = g_new0 (FpcDisumDiagnostics, 1);

  g_atomic_ref_count_init (&self->ref_count);
  self->run_id = g_uuid_string_random ();
  self->phase = FPC_DISUM_DIAGNOSTIC_PHASE_NONE;

  return self;
}

FpcDisumDiagnostics *
fpc_disum_diagnostics_ref (FpcDisumDiagnostics *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  g_atomic_ref_count_inc (&self->ref_count);

  return self;
}

void
fpc_disum_diagnostics_unref (FpcDisumDiagnostics *self)
{
  if (self == NULL)
    return;

  if (g_atomic_ref_count_dec (&self->ref_count))
    {
      g_free (self->run_id);
      g_free (self);
    }
}

const gchar *
fpc_disum_diagnostics_run_id (const FpcDisumDiagnostics *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return self->run_id;
}

guint64
fpc_disum_diagnostics_next_op_id (FpcDisumDiagnostics *self)
{
  g_return_val_if_fail (self != NULL, 0);

  return ++self->next_op_id;
}

void
fpc_disum_diagnostics_set_observer (
    FpcDisumDiagnostics *self, FpcDisumDiagnosticFunc observer, gpointer user_data)
{
  g_return_if_fail (self != NULL);

  self->observer = observer;
  self->observer_data = user_data;
}

void
fpc_disum_diagnostics_set_phase (FpcDisumDiagnostics *self, FpcDisumDiagnosticPhase phase)
{
  g_return_if_fail (self != NULL);

  self->phase = phase;
}

void
fpc_disum_diagnostics_emit (FpcDisumDiagnostics *self, const FpcDisumDiagnosticRecord *record)
{
  FpcDisumDiagnosticRecord copy;

  g_return_if_fail (self != NULL);
  g_return_if_fail (record != NULL);

  if (self->observer == NULL)
    return;

  copy = *record;
  if (copy.phase == FPC_DISUM_DIAGNOSTIC_PHASE_NONE)
    copy.phase = self->phase;
  if (copy.t_us == 0)
    copy.t_us = g_get_monotonic_time ();

  self->observer (&copy, self->observer_data);
}

const gchar *
fpc_disum_diagnostic_kind_name (FpcDisumDiagnosticKind kind)
{
  switch (kind)
    {
    case FPC_DISUM_DIAGNOSTIC_KIND_SUBMIT:
      return "submit";
    case FPC_DISUM_DIAGNOSTIC_KIND_COMPLETE:
      return "complete";
    case FPC_DISUM_DIAGNOSTIC_KIND_CANCEL_REQUEST:
      return "cancel-request";
    case FPC_DISUM_DIAGNOSTIC_KIND_CANCEL_COMPLETE:
      return "cancel-complete";
    case FPC_DISUM_DIAGNOSTIC_KIND_EVENT:
      return "event";
    case FPC_DISUM_DIAGNOSTIC_KIND_CLEANUP:
      return "cleanup";
    case FPC_DISUM_DIAGNOSTIC_KIND_CUE_REQUEST:
      return "cue-request";
    case FPC_DISUM_DIAGNOSTIC_KIND_ROUND_RESULT:
      return "round-result";
    case FPC_DISUM_DIAGNOSTIC_KIND_IDENTITY:
      return "identity";
    }

  return "unknown";
}

const gchar *
fpc_disum_diagnostic_phase_name (FpcDisumDiagnosticPhase phase)
{
  switch (phase)
    {
    case FPC_DISUM_DIAGNOSTIC_PHASE_NONE:
      return NULL;
    case FPC_DISUM_DIAGNOSTIC_PHASE_INIT:
      return "init";
    case FPC_DISUM_DIAGNOSTIC_PHASE_REBIND:
      return "rebind";
    case FPC_DISUM_DIAGNOSTIC_PHASE_HANDSHAKE:
      return "handshake";
    case FPC_DISUM_DIAGNOSTIC_PHASE_WAIT_TOUCH:
      return "wait-touch";
    case FPC_DISUM_DIAGNOSTIC_PHASE_IMAGE:
      return "image";
    case FPC_DISUM_DIAGNOSTIC_PHASE_QUIESCING:
      return "quiescing";
    case FPC_DISUM_DIAGNOSTIC_PHASE_CLEANUP:
      return "cleanup";
    case FPC_DISUM_DIAGNOSTIC_PHASE_DONE:
      return "done";
    }

  return NULL;
}

const gchar *
fpc_disum_transport_status_name (FpcDisumTransportStatus status)
{
  switch (status)
    {
    case FPC_DISUM_TRANSPORT_OK:
      return "ok";
    case FPC_DISUM_TRANSPORT_ERROR_IO:
      return "io";
    case FPC_DISUM_TRANSPORT_ERROR_CANCELLED:
      return "cancelled";
    case FPC_DISUM_TRANSPORT_TIMEOUT:
      return "timeout";
    case FPC_DISUM_TRANSPORT_ERROR_NO_DEVICE:
      return "no-device";
    case FPC_DISUM_TRANSPORT_ERROR_INTEGRITY_UNKNOWN:
      return "integrity-unknown";
    }

  return NULL;
}

static void
add_plain_string (GVariantBuilder *builder, const gchar *key, const gchar *value)
{
  g_variant_builder_add (builder, "{sv}", key, g_variant_new_string (value != NULL ? value : ""));
}

static void
add_string (GVariantBuilder *builder, const gchar *key, const gchar *value)
{
  g_variant_builder_add (builder, "{sv}", key,
      g_variant_new_maybe (
          G_VARIANT_TYPE_STRING, value != NULL ? g_variant_new_string (value) : NULL));
}

static void
add_uint64 (GVariantBuilder *builder, const gchar *key, gboolean valid, guint64 value)
{
  g_variant_builder_add (builder, "{sv}", key,
      g_variant_new_maybe (G_VARIANT_TYPE_UINT64, valid ? g_variant_new_uint64 (value) : NULL));
}

static void
add_uint32 (GVariantBuilder *builder, const gchar *key, gboolean valid, guint32 value)
{
  g_variant_builder_add (builder, "{sv}", key,
      g_variant_new_maybe (G_VARIANT_TYPE_UINT32, valid ? g_variant_new_uint32 (value) : NULL));
}

static void
add_uint16 (GVariantBuilder *builder, const gchar *key, gboolean valid, guint16 value)
{
  g_variant_builder_add (builder, "{sv}", key,
      g_variant_new_maybe (G_VARIANT_TYPE_UINT16, valid ? g_variant_new_uint16 (value) : NULL));
}

static void
add_byte (GVariantBuilder *builder, const gchar *key, gboolean valid, guint8 value)
{
  g_variant_builder_add (builder, "{sv}", key,
      g_variant_new_maybe (G_VARIANT_TYPE_BYTE, valid ? g_variant_new_byte (value) : NULL));
}

static void
add_int32 (GVariantBuilder *builder, const gchar *key, gboolean valid, gint32 value)
{
  g_variant_builder_add (builder, "{sv}", key,
      g_variant_new_maybe (G_VARIANT_TYPE_INT32, valid ? g_variant_new_int32 (value) : NULL));
}

static void
add_boolean (GVariantBuilder *builder, const gchar *key, gboolean valid, gboolean value)
{
  g_variant_builder_add (builder, "{sv}", key,
      g_variant_new_maybe (G_VARIANT_TYPE_BOOLEAN, valid ? g_variant_new_boolean (value) : NULL));
}

GVariant *
fpc_disum_diagnostics_to_variant (
    const FpcDisumDiagnostics *self, const FpcDisumDiagnosticRecord *record)
{
  GVariantBuilder builder;
  const gchar *direction = NULL;
  const gchar *status = NULL;
  const gchar *native_domain = "none";

  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (record != NULL, NULL);

  if (record->direction_valid)
    direction = record->direction == FPC_DISUM_DIAGNOSTIC_DIRECTION_IN ? "in" : "out";
  if (record->status_valid)
    status = fpc_disum_transport_status_name (record->status);
  if (record->native_valid)
    native_domain = record->native_domain == FPC_DISUM_DIAGNOSTIC_NATIVE_LIBUSB ? "libusb" : "gusb";

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  add_plain_string (&builder, "run_id", self->run_id);
  add_uint64 (&builder, "op_id", record->op_id != 0, record->op_id);
  add_plain_string (&builder, "kind", fpc_disum_diagnostic_kind_name (record->kind));
  add_string (&builder, "phase", fpc_disum_diagnostic_phase_name (record->phase));
  g_variant_builder_add (&builder, "{sv}", "t_us",
      g_variant_new_int64 (record->t_us != 0 ? record->t_us : g_get_monotonic_time ()));
  add_uint64 (&builder, "generation", record->identity_valid && record->generation != 0,
      record->generation);
  add_uint64 (
      &builder, "handle_id", record->identity_valid && record->handle_id != 0, record->handle_id);
  add_byte (&builder, "bus", record->identity_valid, record->bus);
  g_variant_builder_add (&builder, "{sv}", "port_numbers",
      g_variant_new_maybe (G_VARIANT_TYPE ("ay"),
          record->identity_valid ? g_variant_new_fixed_array (
                                       G_VARIANT_TYPE_BYTE, record->ports, record->port_count, 1)
                                 : NULL));
  add_byte (&builder, "address", record->identity_valid, record->address);
  add_uint16 (&builder, "vid", record->identity_valid, record->vid);
  add_uint16 (&builder, "pid", record->identity_valid, record->pid);
  add_uint16 (&builder, "bcd_device", record->identity_valid, record->bcd_device);
  add_string (&builder, "direction", direction);
  add_byte (&builder, "bm_request_type", record->control_valid, record->bm_request_type);
  add_uint16 (&builder, "request", record->control_valid, record->request);
  add_uint16 (&builder, "value", record->control_valid, record->value);
  add_uint16 (&builder, "index", record->control_valid, record->index);
  add_uint64 (
      &builder, "requested_length", record->requested_length_valid, record->requested_length);
  add_uint64 (&builder, "actual_length", record->actual_length_known, record->actual_length);
  add_string (&builder, "status", status);
  add_int32 (&builder, "native_status", record->native_valid, record->native_status);
  add_plain_string (&builder, "native_domain", native_domain);
  add_boolean (&builder, "submitted", record->submitted_valid, record->submitted);
  add_uint32 (&builder, "event_code", record->event_valid, record->event_code);
  add_uint32 (&builder, "total_len", record->event_valid, record->total_len);
  add_uint32 (&builder, "third_field", record->event_valid, record->third_field);
  add_uint64 (&builder, "partial_length", record->partial_valid, record->partial_length);
  add_uint64 (&builder, "discarded_length", record->discarded_valid, record->discarded_length);
  add_string (&builder, "primary_result", record->primary_result);
  add_string (&builder, "cleanup_result", record->cleanup_result);

  return g_variant_builder_end (&builder);
}
