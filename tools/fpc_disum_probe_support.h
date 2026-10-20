/*
 * FPC Disum (10a5:9201) probe state and evidence helpers.
 *
 * Reference: docs/CLEANROOM_SPEC.md appendix C.4.
 *
 * This module is linked by the manual hardware probe and by the offline probe
 * test only.  It contains no device access: the state helpers decide when the
 * operator cue is due, and the output object writes de-sensitised metadata.
 * No image, pixel plane, digest or I/O payload ever passes through here.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>
#include <glib.h>

#include "fp-device.h"
#include "fp-image.h"
#include "fpc_disum_session.h"

G_BEGIN_DECLS

typedef enum
{
  /* A capture must deliver a structurally valid image. */
  PROBE_EXPECT_IMAGE = 0,
  /* The only hardware test in this round: open, ARM, wait for the driver's
   * touch window to expire and clean up.  The operator must not press. */
  PROBE_EXPECT_NO_TOUCH,
  /* The capture is cancelled; it must end as CANCELLED with a completed cleanup. */
  PROBE_EXPECT_CANCELLED,
} ProbeRoundExpectation;

/*
 * Per-round state.  Zero-initialise it, set @expected and, when the round should
 * own its cancellation, @cancellable; clear it when the round is over.  The
 * guard, cancellation and progress sources are plain GLib source ids so the
 * owner can install and remove them without allocating a second object.
 */
typedef struct
{
  ProbeRoundExpectation expected;
  gboolean cue_sent;
  gboolean finger_seen;
  GCancellable *cancellable; /* owned */
  guint guard_id;
  guint cancel_id;
  guint progress_id;
  gint64 arm_complete_us;
} FpcDisumProbeCaptureState;

/*
 * First action decision for the arm-complete signal.  Returns TRUE exactly once
 * when an image round saw a four-byte ARM completion and the cue was not sent.
 * The arm-complete timestamp is recorded for progress only; a three- or
 * zero-byte completion is never a cue and never starts the progress line.
 */
gboolean fpc_disum_probe_on_arm (FpcDisumProbeCaptureState *state, gsize actual_length);

/*
 * First action decision for the finger-status property.  Returns TRUE exactly
 * once when PRESENT is set and was not reported before.  NEEDED alone is never
 * an action: the cue comes from arm-complete, not from this flags value.
 */
gboolean fpc_disum_probe_on_finger_flags (
    FpcDisumProbeCaptureState *state, FpFingerStatusFlags flags);

/* Remove the sources, release the owned cancellable and zero the state. */
void fpc_disum_probe_capture_state_clear (FpcDisumProbeCaptureState *state);

typedef struct
{
  FpcDisumSessionStatus logical_status;
  FpImage *image; /* owned; NULL unless an image was delivered */
  gboolean image_structure_valid;
  gboolean capture_pass;
  gboolean cleanup_pass;
  gboolean completion_seen;
} ProbeRoundResult;

/* Static names used by the one-line round report and the run summary. */
const gchar *fpc_disum_probe_expectation_name (ProbeRoundExpectation expected);
const gchar *fpc_disum_probe_logical_status_name (FpcDisumSessionStatus status);
/* Parse a diagnostics primary_result string; no free-form text is accepted. */
gboolean fpc_disum_probe_status_from_name (
    const gchar *name, FpcDisumSessionStatus *out_status);

/*
 * Whole-run accumulation: a later success never hides an earlier failure.  A
 * run only passes when at least one round was added and every round passed its
 * capture, cleanup and termination checks.
 */
typedef struct _FpcDisumProbeRun FpcDisumProbeRun;

FpcDisumProbeRun *fpc_disum_probe_run_new (void);
void fpc_disum_probe_run_add (FpcDisumProbeRun *run, const ProbeRoundResult *result);
gboolean fpc_disum_probe_run_passed (const FpcDisumProbeRun *run);
void fpc_disum_probe_run_free (FpcDisumProbeRun *run);

/* Two captures differ only when both exist and no byte of the planes matches.
 * The comparison stays in memory; nothing is stored or printed but the boolean. */
gboolean fpc_disum_probe_images_distinct (const FpImage *a, const FpImage *b);

/* Reopen acceptance: both rounds passed and their images are distinct. */
gboolean fpc_disum_probe_reopen_passed (
    const FpcDisumProbeRun *run, const FpImage *first, const FpImage *second);

/*
 * Evidence writer.  @path must not exist: new() creates the directory with mode
 * 0700 and run.json, events.jsonl and operator-feedback.json with mode 0600 and
 * O_EXCL.  Existing paths are refused so a run cannot be appended to.
 */
typedef struct _FpcDisumProbeOutput FpcDisumProbeOutput;

FpcDisumProbeOutput *fpc_disum_probe_output_new (
    const gchar *path, const gchar *run_id, GError **error);

/*
 * Append one diagnostics a{sv} as a JSON line.  Only the whitelisted metadata
 * keys are accepted; unknown keys or non-whitelisted value types are refused
 * with an error and nothing is written.  Values use json-glib escaping.
 */
gboolean fpc_disum_probe_output_write (
    FpcDisumProbeOutput *output, GVariant *diagnostic, GError **error);

/*
 * Write run.json from @summary (a{sv}; unknown keys and types are refused) and
 * operator-feedback.json from @operator_feedback (a{sv}).
 */
gboolean fpc_disum_probe_output_finish (FpcDisumProbeOutput *output, GVariant *summary,
    GVariant *operator_feedback, GError **error);

/* Close every descriptor.  Evidence files are never deleted automatically. */
void fpc_disum_probe_output_free (FpcDisumProbeOutput *output);

G_END_DECLS
