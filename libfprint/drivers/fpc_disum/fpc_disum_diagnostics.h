/*
 * FPC Disum (10a5:9201) de-sensitised completion records.
 *
 * Reference: docs/CLEANROOM_SPEC.md appendix C.4.
 *
 * This module carries typed metadata only.  It never receives, stores or
 * serialises I/O payload bytes; a completion record describes what happened to
 * a transfer, not what it carried.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "fpc_disum_transport.h"

G_BEGIN_DECLS

typedef struct _FpcDisumDiagnostics FpcDisumDiagnostics;

typedef enum
{
  FPC_DISUM_DIAGNOSTIC_PHASE_NONE = 0,
  FPC_DISUM_DIAGNOSTIC_PHASE_INIT,
  FPC_DISUM_DIAGNOSTIC_PHASE_REBIND,
  FPC_DISUM_DIAGNOSTIC_PHASE_HANDSHAKE,
  FPC_DISUM_DIAGNOSTIC_PHASE_WAIT_TOUCH,
  FPC_DISUM_DIAGNOSTIC_PHASE_IMAGE,
  FPC_DISUM_DIAGNOSTIC_PHASE_QUIESCING,
  FPC_DISUM_DIAGNOSTIC_PHASE_CLEANUP,
  FPC_DISUM_DIAGNOSTIC_PHASE_DONE,
} FpcDisumDiagnosticPhase;

typedef enum
{
  FPC_DISUM_DIAGNOSTIC_KIND_SUBMIT = 0,
  FPC_DISUM_DIAGNOSTIC_KIND_COMPLETE,
  FPC_DISUM_DIAGNOSTIC_KIND_CANCEL_REQUEST,
  FPC_DISUM_DIAGNOSTIC_KIND_CANCEL_COMPLETE,
  FPC_DISUM_DIAGNOSTIC_KIND_EVENT,
  FPC_DISUM_DIAGNOSTIC_KIND_CLEANUP,
  FPC_DISUM_DIAGNOSTIC_KIND_CUE_REQUEST,
  FPC_DISUM_DIAGNOSTIC_KIND_ROUND_RESULT,
  FPC_DISUM_DIAGNOSTIC_KIND_IDENTITY,
} FpcDisumDiagnosticKind;

typedef enum
{
  FPC_DISUM_DIAGNOSTIC_DIRECTION_NONE = 0,
  FPC_DISUM_DIAGNOSTIC_DIRECTION_IN,
  FPC_DISUM_DIAGNOSTIC_DIRECTION_OUT,
} FpcDisumDiagnosticDirection;

typedef enum
{
  FPC_DISUM_DIAGNOSTIC_NATIVE_NONE = 0,
  FPC_DISUM_DIAGNOSTIC_NATIVE_LIBUSB,
  FPC_DISUM_DIAGNOSTIC_NATIVE_GUSB,
} FpcDisumDiagnosticNativeDomain;

typedef struct
{
  FpcDisumDiagnosticKind kind;
  FpcDisumDiagnosticPhase phase;
  guint64 op_id;
  gint64 t_us;

  /* Identity snapshot at the time of the record.  generation 0 is unknown. */
  gboolean identity_valid;
  guint64 generation;
  guint64 handle_id;
  guint8 bus;
  guint8 port_count;
  guint8 ports[8];
  guint16 vid;
  guint16 pid;
  guint16 bcd_device;
  guint8 address;

  gboolean direction_valid;
  FpcDisumDiagnosticDirection direction;

  /* Control request fields; unused for bulk transfers. */
  gboolean control_valid;
  guint8 bm_request_type;
  guint16 request;
  guint16 value;
  guint16 index;

  gboolean requested_length_valid;
  gsize requested_length;
  gboolean actual_length_known;
  gsize actual_length;

  gboolean status_valid;
  FpcDisumTransportStatus status;

  gboolean native_valid;
  gint native_status;
  FpcDisumDiagnosticNativeDomain native_domain;

  gboolean submitted_valid;
  gboolean submitted;

  gboolean event_valid;
  guint32 event_code;
  guint32 total_len;
  guint32 third_field;

  gboolean partial_valid;
  gsize partial_length;
  gboolean discarded_valid;
  gsize discarded_length;

  /* Static logical result names (never free-form device text). */
  const gchar *primary_result;
  const gchar *cleanup_result;
} FpcDisumDiagnosticRecord;

typedef void (*FpcDisumDiagnosticFunc) (const FpcDisumDiagnosticRecord *record, gpointer user_data);

FpcDisumDiagnostics *fpc_disum_diagnostics_new (void);
FpcDisumDiagnostics *fpc_disum_diagnostics_ref (FpcDisumDiagnostics *diagnostics);
void fpc_disum_diagnostics_unref (FpcDisumDiagnostics *diagnostics);

const gchar *fpc_disum_diagnostics_run_id (const FpcDisumDiagnostics *diagnostics);
guint64 fpc_disum_diagnostics_next_op_id (FpcDisumDiagnostics *diagnostics);

void fpc_disum_diagnostics_set_observer (
    FpcDisumDiagnostics *diagnostics, FpcDisumDiagnosticFunc observer, gpointer user_data);
void fpc_disum_diagnostics_set_phase (
    FpcDisumDiagnostics *diagnostics, FpcDisumDiagnosticPhase phase);
void fpc_disum_diagnostics_emit (
    FpcDisumDiagnostics *diagnostics, const FpcDisumDiagnosticRecord *record);

/* Full reference, a{sv}, whitelisted fields only. */
GVariant *fpc_disum_diagnostics_to_variant (
    const FpcDisumDiagnostics *diagnostics, const FpcDisumDiagnosticRecord *record);

const gchar *fpc_disum_diagnostic_kind_name (FpcDisumDiagnosticKind kind);
const gchar *fpc_disum_diagnostic_phase_name (FpcDisumDiagnosticPhase phase);
const gchar *fpc_disum_transport_status_name (FpcDisumTransportStatus status);

G_END_DECLS
