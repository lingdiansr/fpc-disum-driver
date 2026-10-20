/*
 * FPC Disum (10a5:9201) probe state and evidence helpers.
 *
 * Reference: docs/CLEANROOM_SPEC.md appendix C.4.
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_probe_support.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include <json-glib/json-glib.h>

/* ------------------------------------------------------------ round state */

gboolean
fpc_disum_probe_on_arm (FpcDisumProbeCaptureState *state, gsize actual_length)
{
  g_return_val_if_fail (state != NULL, FALSE);

  /* Only a four-byte completion is a successful ARM (SPEC §6).  A short
   * completion must never look like a cue. */
  if (actual_length != 4)
    return FALSE;

  state->arm_complete_us = g_get_monotonic_time ();

  if (state->expected != PROBE_EXPECT_IMAGE || state->cue_sent)
    return FALSE;

  state->cue_sent = TRUE;

  return TRUE;
}

gboolean
fpc_disum_probe_on_finger_flags (FpcDisumProbeCaptureState *state, FpFingerStatusFlags flags)
{
  g_return_val_if_fail (state != NULL, FALSE);

  /* NEEDED is not a completion.  PRESENT is reported once and never replays
   * the ARM cue. */
  if ((flags & FP_FINGER_STATUS_PRESENT) == 0 || state->finger_seen)
    return FALSE;

  state->finger_seen = TRUE;

  return TRUE;
}

void
fpc_disum_probe_capture_state_clear (FpcDisumProbeCaptureState *state)
{
  g_return_if_fail (state != NULL);

  if (state->guard_id != 0)
    {
      g_source_remove (state->guard_id);
      state->guard_id = 0;
    }
  if (state->cancel_id != 0)
    {
      g_source_remove (state->cancel_id);
      state->cancel_id = 0;
    }
  if (state->progress_id != 0)
    {
      g_source_remove (state->progress_id);
      state->progress_id = 0;
    }

  g_clear_object (&state->cancellable);

  memset (state, 0, sizeof (*state));
}

/* -------------------------------------------------------------- run result */

struct _FpcDisumProbeRun
{
  guint rounds;
  gboolean passed;
};

FpcDisumProbeRun *
fpc_disum_probe_run_new (void)
{
  FpcDisumProbeRun *run = g_new0 (FpcDisumProbeRun, 1);

  run->passed = TRUE;

  return run;
}

void
fpc_disum_probe_run_add (FpcDisumProbeRun *run, const ProbeRoundResult *result)
{
  g_return_if_fail (run != NULL);
  g_return_if_fail (result != NULL);

  run->rounds++;

  /* A later round never repairs an earlier failure. */
  if (!result->capture_pass || !result->cleanup_pass || !result->completion_seen)
    run->passed = FALSE;
}

gboolean
fpc_disum_probe_run_passed (const FpcDisumProbeRun *run)
{
  g_return_val_if_fail (run != NULL, FALSE);

  return run->rounds > 0 && run->passed;
}

void
fpc_disum_probe_run_free (FpcDisumProbeRun *run)
{
  g_free (run);
}

gboolean
fpc_disum_probe_images_distinct (const FpImage *a, const FpImage *b)
{
  gsize a_len = 0, b_len = 0;
  const guchar *a_data, *b_data;

  if (a == NULL || b == NULL)
    return FALSE;

  if (fp_image_get_width ((FpImage *)a) != fp_image_get_width ((FpImage *)b)
      || fp_image_get_height ((FpImage *)a) != fp_image_get_height ((FpImage *)b))
    return TRUE;

  a_data = fp_image_get_data ((FpImage *)a, &a_len);
  b_data = fp_image_get_data ((FpImage *)b, &b_len);

  if (a_len != b_len)
    return TRUE;

  return memcmp (a_data, b_data, a_len) != 0;
}

gboolean
fpc_disum_probe_reopen_passed (
    const FpcDisumProbeRun *run, const FpImage *first, const FpImage *second)
{
  return fpc_disum_probe_run_passed (run) && fpc_disum_probe_images_distinct (first, second);
}

/* ------------------------------------------------------------ name tables */

static const struct
{
  FpcDisumSessionStatus status;
  const gchar *name;
} logical_status_names[] = {
  { FPC_DISUM_SESSION_OK, "ok" },
  { FPC_DISUM_SESSION_ERROR_INVALID_ARGUMENT, "invalid-argument" },
  { FPC_DISUM_SESSION_ERROR_IO, "io" },
  { FPC_DISUM_SESSION_ERROR_PROTOCOL, "protocol" },
  { FPC_DISUM_SESSION_ERROR_UNEXPECTED_EVENT, "unexpected-event" },
  { FPC_DISUM_SESSION_ERROR_ENVELOPE, "envelope" },
  { FPC_DISUM_SESSION_ERROR_TLS, "tls" },
  { FPC_DISUM_SESSION_ERROR_NO_TOUCH, "no-touch" },
  { FPC_DISUM_SESSION_ERROR_NO_IMAGE, "no-image" },
  { FPC_DISUM_SESSION_ERROR_ARM_RESULT, "arm-result" },
  { FPC_DISUM_SESSION_ERROR_CANCELLED, "cancelled" },
  { FPC_DISUM_SESSION_ERROR_CONNECTION_CHANGED, "connection-changed" },
  { FPC_DISUM_SESSION_ERROR_UNSUPPORTED_RESET_RECOVERY, "unsupported-reset-recovery" },
};

const gchar *
fpc_disum_probe_logical_status_name (FpcDisumSessionStatus status)
{
  for (gsize i = 0; i < G_N_ELEMENTS (logical_status_names); i++)
    {
      if (logical_status_names[i].status == status)
        return logical_status_names[i].name;
    }

  return "invalid-argument";
}

gboolean
fpc_disum_probe_status_from_name (const gchar *name, FpcDisumSessionStatus *out_status)
{
  g_return_val_if_fail (out_status != NULL, FALSE);

  if (name == NULL)
    return FALSE;

  for (gsize i = 0; i < G_N_ELEMENTS (logical_status_names); i++)
    {
      if (g_strcmp0 (logical_status_names[i].name, name) == 0)
        {
          *out_status = logical_status_names[i].status;
          return TRUE;
        }
    }

  return FALSE;
}

const gchar *
fpc_disum_probe_expectation_name (ProbeRoundExpectation expected)
{
  switch (expected)
    {
    case PROBE_EXPECT_IMAGE:
      return "image";
    case PROBE_EXPECT_NO_TOUCH:
      return "no-touch";
    case PROBE_EXPECT_CANCELLED:
      return "cancelled";
    }

  return "unknown";
}

/* ------------------------------------------------------- variant -> JSON */

/*
 * The evidence writer accepts a deliberately small, typed subset.  It is fed
 * either a diagnostic a{sv} (whitelisted metadata) or the run summary.  Any
 * type outside this subset is refused; the caller has already validated the
 * key names.
 */

static JsonNode *node_from_value (GVariant *value, GError **error);

static JsonNode *
node_from_dict (GVariant *dict, GError **error)
{
  GVariantIter iter;
  gchar *key;
  GVariant *child;
  JsonObject *object = json_object_new ();
  JsonNode *node;

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_next (&iter, "{sv}", &key, &child))
    {
      JsonNode *member = node_from_value (child, error);

      g_variant_unref (child);
      if (member == NULL)
        {
          g_free (key);
          json_object_unref (object);
          return NULL;
        }

      json_object_set_member (object, key, member);
      g_free (key);
    }

  node = json_node_alloc ();
  json_node_init_object (node, object);
  json_object_unref (object);

  return node;
}

static JsonNode *
node_from_array (GVariant *array, GError **error)
{
  GVariantIter iter;
  GVariant *child;
  JsonArray *json_array = json_array_new ();
  JsonNode *node;

  g_variant_iter_init (&iter, array);
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      GVariant *value = child;
      JsonNode *element;

      /* "av" holds variants; every other supported array holds plain values. */
      if (g_variant_is_of_type (child, G_VARIANT_TYPE_VARIANT))
        value = g_variant_get_variant (child);

      element = node_from_value (value, error);

      if (value != child)
        g_variant_unref (value);
      g_variant_unref (child);

      if (element == NULL)
        {
          json_array_unref (json_array);
          return NULL;
        }

      json_array_add_element (json_array, element);
    }

  node = json_node_alloc ();
  json_node_init_array (node, json_array);
  json_array_unref (json_array);

  return node;
}

static JsonNode *
node_from_value (GVariant *value, GError **error)
{
  const GVariantType *type = g_variant_get_type (value);
  JsonNode *node;

  /* a{sv} is also an array type; it must be recognised as an object first. */
  if (g_variant_type_equal (type, G_VARIANT_TYPE_VARDICT))
    return node_from_dict (value, error);

  if (g_variant_type_is_maybe (type))
    {
      GVariant *child = g_variant_get_maybe (value);

      if (child == NULL)
        {
          node = json_node_alloc ();
          json_node_init_null (node);
          return node;
        }

      node = node_from_value (child, error);
      g_variant_unref (child);
      return node;
    }

  if (g_variant_type_is_array (type))
    {
      const GVariantType *element = g_variant_type_element (type);

      if (g_variant_type_equal (element, G_VARIANT_TYPE_VARDICT)
          || g_variant_type_equal (element, G_VARIANT_TYPE_VARIANT)
          || g_variant_type_equal (element, G_VARIANT_TYPE_BYTE))
        return node_from_array (value, error);

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
          "Unsupported evidence array type '%s'", g_variant_type_peek_string (type));
      return NULL;
    }

  node = json_node_alloc ();

  if (g_variant_type_equal (type, G_VARIANT_TYPE_STRING))
    {
      json_node_init_string (node, g_variant_get_string (value, NULL));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_BOOLEAN))
    {
      json_node_init_boolean (node, g_variant_get_boolean (value));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_BYTE))
    {
      json_node_init_int (node, g_variant_get_byte (value));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_INT16))
    {
      json_node_init_int (node, g_variant_get_int16 (value));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_UINT16))
    {
      json_node_init_int (node, g_variant_get_uint16 (value));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_INT32))
    {
      json_node_init_int (node, g_variant_get_int32 (value));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_UINT32))
    {
      json_node_init_int (node, g_variant_get_uint32 (value));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_INT64))
    {
      json_node_init_int (node, g_variant_get_int64 (value));
    }
  else if (g_variant_type_equal (type, G_VARIANT_TYPE_UINT64))
    {
      json_node_init_int (node, (gint64)g_variant_get_uint64 (value));
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
          "Unsupported evidence value type '%s'", g_variant_type_peek_string (type));
      json_node_free (node);
      return NULL;
    }

  return node;
}

/* ---------------------------------------------------------- field specs */

typedef struct
{
  const gchar *name;
  const GVariantType *type;
} FieldSpec;

static gboolean
validate_dict (
    GVariant *dict, const FieldSpec *specs, gsize n_specs, gboolean require_all, GError **error)
{
  GVariantIter iter;
  gchar *key;
  GVariant *child;
  gboolean seen[64] = { FALSE };
  gsize matched = 0;

  g_return_val_if_fail (n_specs <= G_N_ELEMENTS (seen), FALSE);

  if (!g_variant_is_of_type (dict, G_VARIANT_TYPE_VARDICT))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Evidence object must be a{sv}");
      return FALSE;
    }

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_next (&iter, "{sv}", &key, &child))
    {
      gsize i;

      for (i = 0; i < n_specs; i++)
        {
          if (g_strcmp0 (specs[i].name, key) == 0)
            break;
        }

      if (i == n_specs)
        {
          g_set_error (
              error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unknown evidence field '%s'", key);
          g_free (key);
          g_variant_unref (child);
          return FALSE;
        }

      if (seen[i])
        {
          g_set_error (
              error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Duplicate evidence field '%s'", key);
          g_free (key);
          g_variant_unref (child);
          return FALSE;
        }

      seen[i] = TRUE;
      matched++;

      if (!g_variant_is_of_type (child, specs[i].type))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
              "Evidence field '%s' has type '%s', expected '%s'", key,
              g_variant_type_peek_string (g_variant_get_type (child)),
              g_variant_type_peek_string (specs[i].type));
          g_free (key);
          g_variant_unref (child);
          return FALSE;
        }

      g_free (key);
      g_variant_unref (child);
    }

  if (require_all && matched != n_specs)
    {
      g_set_error (
          error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Evidence object is missing required fields");
      return FALSE;
    }

  return TRUE;
}

/* Appendix C.4 whitelist.  No payload, image, digest or free-form device text. */
static const FieldSpec diagnostic_fields[] = {
  { "run_id", G_VARIANT_TYPE_STRING },
  { "op_id", (const GVariantType *)"mt" },
  { "kind", G_VARIANT_TYPE_STRING },
  { "phase", (const GVariantType *)"ms" },
  { "t_us", G_VARIANT_TYPE_INT64 },
  { "generation", (const GVariantType *)"mt" },
  { "handle_id", (const GVariantType *)"mt" },
  { "bus", (const GVariantType *)"my" },
  { "port_numbers", (const GVariantType *)"may" },
  { "address", (const GVariantType *)"my" },
  { "vid", (const GVariantType *)"mq" },
  { "pid", (const GVariantType *)"mq" },
  { "bcd_device", (const GVariantType *)"mq" },
  { "direction", (const GVariantType *)"ms" },
  { "bm_request_type", (const GVariantType *)"my" },
  { "request", (const GVariantType *)"mq" },
  { "value", (const GVariantType *)"mq" },
  { "index", (const GVariantType *)"mq" },
  { "requested_length", (const GVariantType *)"mt" },
  { "actual_length", (const GVariantType *)"mt" },
  { "status", (const GVariantType *)"ms" },
  { "native_status", (const GVariantType *)"mi" },
  { "native_domain", G_VARIANT_TYPE_STRING },
  { "submitted", (const GVariantType *)"mb" },
  { "event_code", (const GVariantType *)"mu" },
  { "total_len", (const GVariantType *)"mu" },
  { "third_field", (const GVariantType *)"mu" },
  { "partial_length", (const GVariantType *)"mt" },
  { "discarded_length", (const GVariantType *)"mt" },
  { "primary_result", (const GVariantType *)"ms" },
  { "cleanup_result", (const GVariantType *)"ms" },
};

static const FieldSpec binary_fields[] = {
  { "path", G_VARIANT_TYPE_STRING },
  { "size", G_VARIANT_TYPE_UINT64 },
  { "sha256", G_VARIANT_TYPE_STRING },
};

static const FieldSpec round_fields[] = {
  { "round", G_VARIANT_TYPE_INT32 },
  { "logical_status", G_VARIANT_TYPE_STRING },
  { "capture_pass", G_VARIANT_TYPE_BOOLEAN },
  { "cleanup_pass", G_VARIANT_TYPE_BOOLEAN },
  { "image_width", G_VARIANT_TYPE_INT32 },
  { "image_height", G_VARIANT_TYPE_INT32 },
  { "image_bytes", G_VARIANT_TYPE_UINT64 },
  { "image_structure_valid", G_VARIANT_TYPE_BOOLEAN },
  { "image_distinct", G_VARIANT_TYPE_BOOLEAN },
};

static const FieldSpec summary_fields[] = {
  { "run_id", G_VARIANT_TYPE_STRING },
  { "spec_sha256", G_VARIANT_TYPE_STRING },
  { "probe_binary", G_VARIANT_TYPE_VARDICT },
  { "loaded_library", G_VARIANT_TYPE_VARDICT },
  { "rounds", (const GVariantType *)"aa{sv}" },
  { "evidence_complete", G_VARIANT_TYPE_BOOLEAN },
};

static const FieldSpec feedback_fields[] = {
  { "pressed_after_cue", (const GVariantType *)"ms" },
  { "held_until_end", (const GVariantType *)"ms" },
  { "perceived_cue_us", (const GVariantType *)"mx" },
  { "press_us", (const GVariantType *)"mx" },
  { "release_us", (const GVariantType *)"mx" },
};

static gboolean
validate_rounds (GVariant *rounds, GError **error)
{
  GVariantIter iter;
  GVariant *entry;

  if (!g_variant_is_of_type (rounds, G_VARIANT_TYPE ("aa{sv}")))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Evidence rounds must be aa{sv}");
      return FALSE;
    }

  g_variant_iter_init (&iter, rounds);
  while ((entry = g_variant_iter_next_value (&iter)) != NULL)
    {
      gboolean ok = validate_dict (entry, round_fields, G_N_ELEMENTS (round_fields), TRUE, error);

      g_variant_unref (entry);

      if (!ok)
        return FALSE;
    }

  return TRUE;
}

static gboolean
validate_summary (GVariant *summary, GError **error)
{
  GVariant *value;

  if (!validate_dict (summary, summary_fields, G_N_ELEMENTS (summary_fields), TRUE, error))
    return FALSE;

  value = g_variant_lookup_value (summary, "probe_binary", NULL);
  if (value == NULL
      || !validate_dict (value, binary_fields, G_N_ELEMENTS (binary_fields), TRUE, error))
    {
      g_clear_pointer (&value, g_variant_unref);
      return FALSE;
    }
  g_variant_unref (value);

  value = g_variant_lookup_value (summary, "loaded_library", NULL);
  if (value == NULL
      || !validate_dict (value, binary_fields, G_N_ELEMENTS (binary_fields), TRUE, error))
    {
      g_clear_pointer (&value, g_variant_unref);
      return FALSE;
    }
  g_variant_unref (value);

  value = g_variant_lookup_value (summary, "rounds", NULL);
  if (value == NULL || !validate_rounds (value, error))
    {
      g_clear_pointer (&value, g_variant_unref);
      return FALSE;
    }
  g_variant_unref (value);

  return TRUE;
}

/* -------------------------------------------------------------- writer */

struct _FpcDisumProbeOutput
{
  gchar *run_id;
  gchar *path;
  gint events_fd;
  gint run_fd;
  gint feedback_fd;
  gboolean broken;
  gboolean finished;
};

static gboolean
write_all (gint fd, const gchar *data, gsize len, GError **error)
{
  gsize written = 0;

  while (written < len)
    {
      ssize_t ret = write (fd, data + written, len - written);

      if (ret < 0)
        {
          if (errno == EINTR)
            continue;

          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
              "Could not write evidence: %s", g_strerror (errno));
          return FALSE;
        }

      if (ret == 0)
        {
          g_set_error (
              error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not write evidence: short write");
          return FALSE;
        }

      written += (gsize)ret;
    }

  return TRUE;
}

static gboolean
write_json_node (gint fd, JsonNode *node, gboolean newline, GError **error)
{
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autofree gchar *json = NULL;
  g_autofree gchar *line = NULL;

  json_generator_set_root (generator, node);
  json = json_generator_to_data (generator, NULL);

  if (json == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not serialise evidence");
      return FALSE;
    }

  line = newline ? g_strconcat (json, "\n", NULL) : g_strdup (json);

  return write_all (fd, line, strlen (line), error);
}

static gint
create_evidence_file (const gchar *path, const gchar *name, GError **error)
{
  g_autofree gchar *file = g_build_filename (path, name, NULL);
  gint fd;

  fd = open (file, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not create '%s': %s",
          file, g_strerror (errno));
      return -1;
    }

  if (fchmod (fd, 0600) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not protect '%s': %s",
          file, g_strerror (errno));
      close (fd);
      return -1;
    }

  return fd;
}

FpcDisumProbeOutput *
fpc_disum_probe_output_new (const gchar *path, const gchar *run_id, GError **error)
{
  FpcDisumProbeOutput *output;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (path == NULL || *path == '\0' || run_id == NULL || *run_id == '\0')
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
          "An output directory and run identifier are required");
      return NULL;
    }

  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_set_error (
          error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The output directory '%s' already exists", path);
      return NULL;
    }

  /* A fresh directory: never append to a previous run's evidence. */
  if (g_mkdir (path, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not create '%s': %s",
          path, g_strerror (errno));
      return NULL;
    }

  /* mkdir is masked by the umask; the evidence directory must be owner-only. */
  if (g_chmod (path, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not protect '%s': %s",
          path, g_strerror (errno));
      return NULL;
    }

  output = g_new0 (FpcDisumProbeOutput, 1);
  output->run_id = g_strdup (run_id);
  output->path = g_strdup (path);
  output->events_fd = -1;
  output->run_fd = -1;
  output->feedback_fd = -1;

  output->run_fd = create_evidence_file (path, "run.json", error);
  if (output->run_fd < 0)
    goto fail;

  output->events_fd = create_evidence_file (path, "events.jsonl", error);
  if (output->events_fd < 0)
    goto fail;

  output->feedback_fd = create_evidence_file (path, "operator-feedback.json", error);
  if (output->feedback_fd < 0)
    goto fail;

  return output;

fail:
  fpc_disum_probe_output_free (output);
  return NULL;
}

gboolean
fpc_disum_probe_output_write (FpcDisumProbeOutput *output, GVariant *diagnostic, GError **error)
{
  g_autoptr (JsonNode) node = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (output == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No evidence output");
      return FALSE;
    }

  if (output->finished || output->broken)
    {
      output->broken = TRUE;
      g_set_error (
          error, G_IO_ERROR, G_IO_ERROR_FAILED, "The evidence output is no longer writable");
      return FALSE;
    }

  /* A rejected record is not written; the output stays usable so the caller
   * can see every rejected attempt. */
  if (diagnostic == NULL
      || !validate_dict (
          diagnostic, diagnostic_fields, G_N_ELEMENTS (diagnostic_fields), FALSE, error))
    return FALSE;

  node = node_from_value (diagnostic, error);
  if (node == NULL)
    return FALSE;

  if (!write_json_node (output->events_fd, node, TRUE, error))
    {
      output->broken = TRUE;
      return FALSE;
    }

  return TRUE;
}

gboolean
fpc_disum_probe_output_finish (
    FpcDisumProbeOutput *output, GVariant *summary, GVariant *operator_feedback, GError **error)
{
  g_autoptr (JsonNode) summary_node = NULL;
  g_autoptr (JsonNode) feedback_node = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (output == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No evidence output");
      return FALSE;
    }

  if (output->finished)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Evidence was already finished");
      return FALSE;
    }

  if (output->broken)
    {
      g_set_error (
          error, G_IO_ERROR, G_IO_ERROR_FAILED, "Evidence writing failed earlier in the run");
      return FALSE;
    }

  if (summary == NULL || !validate_summary (summary, error))
    return FALSE;

  if (operator_feedback == NULL
      || !validate_dict (
          operator_feedback, feedback_fields, G_N_ELEMENTS (feedback_fields), TRUE, error))
    return FALSE;

  feedback_node = node_from_value (operator_feedback, error);
  if (feedback_node == NULL)
    return FALSE;

  summary_node = node_from_value (summary, error);
  if (summary_node == NULL)
    return FALSE;

  /* run.json carries the summary plus the operator's own report. */
  json_object_set_member (
      json_node_get_object (summary_node), "operator_feedback", json_node_copy (feedback_node));

  if (!write_json_node (output->run_fd, summary_node, FALSE, error))
    return FALSE;

  if (!write_json_node (output->feedback_fd, feedback_node, FALSE, error))
    return FALSE;

  output->finished = TRUE;

  if (output->events_fd >= 0)
    {
      close (output->events_fd);
      output->events_fd = -1;
    }
  if (output->run_fd >= 0)
    {
      close (output->run_fd);
      output->run_fd = -1;
    }
  if (output->feedback_fd >= 0)
    {
      close (output->feedback_fd);
      output->feedback_fd = -1;
    }

  return TRUE;
}

void
fpc_disum_probe_output_free (FpcDisumProbeOutput *output)
{
  if (output == NULL)
    return;

  if (output->events_fd >= 0)
    close (output->events_fd);
  if (output->run_fd >= 0)
    close (output->run_fd);
  if (output->feedback_fd >= 0)
    close (output->feedback_fd);

  g_free (output->run_id);
  g_free (output->path);
  g_free (output);
}
