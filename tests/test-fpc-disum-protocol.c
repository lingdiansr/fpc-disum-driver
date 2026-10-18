/*
 * FPC Disum protocol framing tests.
 *
 * The vector corpus is generated from docs/CLEANROOM_TEST_VECTORS.json by
 * tools/fpc_disum_vectors.py; this test executes the real C parser against the
 * reconstructed reads and checks the recorded expectations.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>

#include "fpc-disum-corpus.h"
#include "fpc_disum_proto.h"

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (FpcDisumParser, fpc_disum_parser_clear)

typedef struct
{
  guint32 code;
  guint32 total_len;
  guint32 third_field;
  gsize payload_len;
  gchar *payload_sha256;
} ObservedFrame;

typedef struct
{
  GPtrArray *frames;
  guint accepted_images;
  gboolean got_image;
  FpcDisumImage image;
  /* Optional cross-check of the extracted plane against the vector wire. */
  const guint8 *expected_message;
  gboolean pixels_checked;
} Collector;

static void
write_be32 (guint8 *data, guint32 value)
{
  data[0] = (value >> 24) & 0xff;
  data[1] = (value >> 16) & 0xff;
  data[2] = (value >> 8) & 0xff;
  data[3] = value & 0xff;
}

static void
observed_frame_free (gpointer data)
{
  ObservedFrame *frame = data;

  g_free (frame->payload_sha256);
  g_free (frame);
}

static void
collect_frame (const FpcDisumFrame *frame, gpointer user_data)
{
  Collector *collector = user_data;
  ObservedFrame *observed;

  if (fpc_disum_frame_is_image (frame))
    {
      collector->accepted_images++;

      if (fpc_disum_frame_get_image (frame, &collector->image))
        {
          collector->got_image = TRUE;

          if (collector->expected_message != NULL)
            {
              g_assert_cmpuint (collector->image.width, ==, FPC_DISUM_IMAGE_WIDTH);
              g_assert_cmpuint (collector->image.height, ==, FPC_DISUM_IMAGE_HEIGHT);
              g_assert_cmpuint (collector->image.stride, ==, FPC_DISUM_IMAGE_STRIDE);
              g_assert_cmpuint (collector->image.pixel_len, ==, FPC_DISUM_IMAGE_PIXELS);
              g_assert_cmpmem (collector->image.pixels, collector->image.pixel_len,
                  collector->expected_message + FPC_DISUM_IMAGE_PIXEL_OFFSET,
                  FPC_DISUM_IMAGE_PIXELS);
              collector->pixels_checked = TRUE;
            }
        }
    }

  observed = g_new0 (ObservedFrame, 1);
  observed->code = frame->code;
  observed->total_len = frame->total_len;
  observed->third_field = frame->third_field;
  observed->payload_len = frame->payload_len;
  /* The payload is only borrowed for the duration of this callback. */
  observed->payload_sha256
      = g_compute_checksum_for_data (G_CHECKSUM_SHA256, frame->payload, frame->payload_len);
  g_ptr_array_add (collector->frames, observed);
}

static gsize
observed_consumed (Collector *collector)
{
  gsize consumed = 0;

  for (guint i = 0; i < collector->frames->len; i++)
    consumed += ((ObservedFrame *)g_ptr_array_index (collector->frames, i))->total_len;

  return consumed;
}

static void
test_event_vector (gconstpointer data)
{
  const FpcDisumCorpusEventVector *vector = data;
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  gsize offset = 0;
  gsize timeouts = 0;
  gboolean stopped = FALSE;

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_EVENT, collect_frame, &collector);

  for (guint i = 0; i < vector->n_chunks; i++)
    {
      const FpcDisumCorpusChunk *chunk = &vector->chunks[i];
      gsize available;

      if (chunk->is_timeout)
        {
          timeouts++;
          continue;
        }

      /* A read never returns more bytes than the wire still holds. */
      available = MIN (chunk->bytes, vector->wire_len - offset);
      if (!fpc_disum_parser_feed (&parser, vector->wire + offset, available))
        {
          stopped = TRUE;
          break;
        }
      offset += available;
    }

  if (!stopped)
    timeouts++; /* the final read of a successful stream also times out */

  g_assert_cmpint (g_strcmp0 (fpc_disum_parser_error (&parser), vector->error), ==, 0);
  g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, vector->held_partial_bytes);
  g_assert_cmpuint (timeouts, ==, vector->timeouts);
  g_assert_cmpuint (collector.frames->len, ==, vector->n_frames);
  g_assert_cmpuint (collector.accepted_images, ==, 0);

  for (guint i = 0; i < vector->n_frames; i++)
    {
      const FpcDisumCorpusFrame *expected = &vector->frames[i];
      ObservedFrame *observed = g_ptr_array_index (collector.frames, i);

      g_assert_cmpuint (observed->code, ==, expected->code);
      g_assert_cmpuint (observed->total_len, ==, expected->total_len);
      g_assert_cmpuint (observed->third_field, ==, expected->third_field);
      g_assert_cmpuint (observed->payload_len, ==, expected->payload_len);
      g_assert_cmpstr (observed->payload_sha256, ==, expected->payload_sha256);
    }

  g_ptr_array_unref (collector.frames);
}

static void
test_application_vector (gconstpointer data)
{
  const FpcDisumCorpusApplicationVector *vector = data;
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  gsize offset = 0;
  gsize first_consumed = 0;
  gsize first_pending = 0;

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  collector.expected_message = vector->wire;
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_APPLICATION, collect_frame, &collector);

  for (guint i = 0; i < vector->n_feeds; i++)
    {
      gsize available = MIN (vector->feeds[i], vector->wire_len - offset);

      g_assert_true (fpc_disum_parser_feed (&parser, vector->wire + offset, available));
      offset += available;

      if (i == 0)
        {
          first_consumed = observed_consumed (&collector);
          first_pending = fpc_disum_parser_pending (&parser);
        }
    }

  g_assert_cmpuint (first_consumed, ==, vector->consumed_after_first_feed);
  g_assert_cmpuint (first_pending, ==, vector->pending_after_first_feed);
  g_assert_cmpuint (collector.frames->len, ==, vector->n_messages);
  g_assert_cmpuint (collector.accepted_images, ==, vector->accepted_as_image ? 1 : 0);
  g_assert_cmpint (collector.got_image, ==, vector->accepted_as_image);

  if (vector->has_layout)
    {
      /* The implementation constants must match the geometry the vector declares.
       */
      g_assert_cmpuint (FPC_DISUM_IMAGE_PIXEL_OFFSET, ==, vector->pixel_offset);
      g_assert_cmpuint (FPC_DISUM_IMAGE_PIXELS, ==, vector->pixels);
      g_assert_cmpuint (FPC_DISUM_IMAGE_STRIDE, ==, vector->stride);
      g_assert_cmpuint (vector->pixel_offset + vector->pixels, ==, vector->wire_len);
    }

  for (guint i = 0; i < vector->n_messages; i++)
    {
      const FpcDisumCorpusMessage *expected = &vector->messages[i];
      ObservedFrame *observed = g_ptr_array_index (collector.frames, i);

      g_assert_cmpuint (observed->code, ==, expected->code);
      g_assert_cmpuint (observed->total_len, ==, expected->total_len);
      g_assert_cmpuint (observed->payload_len, ==, expected->payload_len);
    }

  g_ptr_array_unref (collector.frames);
}

static void
test_partial_frame_retained (void)
{
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  guint8 message[20] = { 0 };

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_EVENT, collect_frame, &collector);

  /* code 6, total length 20, third field 0 */
  write_be32 (message, 6);
  write_be32 (message + 4, sizeof (message));

  g_assert_true (fpc_disum_parser_feed (&parser, message, FPC_DISUM_FRAME_HEADER_LEN));
  g_assert_null (fpc_disum_parser_error (&parser));
  g_assert_cmpuint (collector.frames->len, ==, 0);
  g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, FPC_DISUM_FRAME_HEADER_LEN);

  g_assert_true (fpc_disum_parser_feed (&parser, message + FPC_DISUM_FRAME_HEADER_LEN,
      sizeof (message) - FPC_DISUM_FRAME_HEADER_LEN));
  g_assert_null (fpc_disum_parser_error (&parser));
  g_assert_cmpuint (collector.frames->len, ==, 1);
  g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, 0);

  g_ptr_array_unref (collector.frames);
}

static void
test_invalid_length_reported_early (void)
{
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  const guint8 probe[FPC_DISUM_FRAME_LENGTH_PROBE_LEN]
      = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0b };

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_EVENT, collect_frame, &collector);

  /* Five bytes cannot carry the length field, so nothing is decided yet. */
  g_assert_true (fpc_disum_parser_feed (&parser, probe, 5));
  g_assert_null (fpc_disum_parser_error (&parser));
  g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, 5);

  /* The length is rejected as soon as its four bytes arrive, without a full
   * header. */
  g_assert_false (fpc_disum_parser_feed (&parser, probe + 5, 3));
  g_assert_cmpint (
      g_strcmp0 (fpc_disum_parser_error (&parser), "ProtocolError: invalid event length 11"), ==,
      0);
  g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, FPC_DISUM_FRAME_LENGTH_PROBE_LEN);
  g_assert_cmpuint (collector.frames->len, ==, 0);

  g_ptr_array_unref (collector.frames);
}

static void
test_stopped_parser_ignores_further_data (void)
{
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  g_autofree guint8 *image = g_malloc0 (FPC_DISUM_IMAGE_TOTAL_LEN);
  const guint8 bad[FPC_DISUM_FRAME_LENGTH_PROBE_LEN]
      = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0b };

  write_be32 (image, FPC_DISUM_IMAGE_CODE);
  write_be32 (image + 4, FPC_DISUM_IMAGE_TOTAL_LEN);
  write_be32 (image + 8, 0);

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_EVENT, collect_frame, &collector);

  g_assert_false (fpc_disum_parser_feed (&parser, bad, sizeof (bad)));

  /* A complete, valid image arriving after the failure must not be delivered.
   */
  g_assert_false (fpc_disum_parser_feed (&parser, image, FPC_DISUM_IMAGE_TOTAL_LEN));
  g_assert_cmpuint (collector.frames->len, ==, 0);
  g_assert_cmpuint (collector.accepted_images, ==, 0);
  g_assert_false (collector.got_image);
  g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, sizeof (bad));
  g_assert_cmpstr (fpc_disum_parser_error (&parser), !=, NULL);

  g_ptr_array_unref (collector.frames);
}

static void
test_capture_accepts_only_image_frames (void)
{
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  g_autofree guint8 *image = g_malloc0 (FPC_DISUM_IMAGE_TOTAL_LEN);
  g_autofree guint8 *other_code = g_malloc0 (FPC_DISUM_IMAGE_TOTAL_LEN);
  guint8 short_message[22] = { 0 };

  write_be32 (image, FPC_DISUM_IMAGE_CODE);
  write_be32 (image + 4, FPC_DISUM_IMAGE_TOTAL_LEN);
  write_be32 (other_code, 7);
  write_be32 (other_code + 4, FPC_DISUM_IMAGE_TOTAL_LEN);
  write_be32 (short_message, FPC_DISUM_IMAGE_CODE);
  write_be32 (short_message + 4, sizeof (short_message));

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_APPLICATION, collect_frame, &collector);

  g_assert_true (fpc_disum_parser_feed (&parser, image, FPC_DISUM_IMAGE_TOTAL_LEN));
  g_assert_true (fpc_disum_parser_feed (&parser, other_code, FPC_DISUM_IMAGE_TOTAL_LEN));
  g_assert_true (fpc_disum_parser_feed (&parser, short_message, sizeof (short_message)));

  /* The framing layer delivers every well-formed message ... */
  g_assert_cmpuint (collector.frames->len, ==, 3);
  /* ... but only "code 8 with the declared length" is an image. */
  g_assert_cmpuint (collector.accepted_images, ==, 1);
  g_assert_true (collector.got_image);
  g_assert_cmpuint (
      ((ObservedFrame *)g_ptr_array_index (collector.frames, 0))->code, ==, FPC_DISUM_IMAGE_CODE);
  g_assert_cmpuint (((ObservedFrame *)g_ptr_array_index (collector.frames, 1))->code, ==, 7);
  g_assert_cmpuint (((ObservedFrame *)g_ptr_array_index (collector.frames, 2))->total_len, ==,
      sizeof (short_message));

  g_ptr_array_unref (collector.frames);
}

static void
test_image_plane_at_declared_offset (void)
{
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  g_autofree guint8 *message = g_malloc0 (FPC_DISUM_IMAGE_TOTAL_LEN);

  for (gsize i = 0; i < FPC_DISUM_IMAGE_TOTAL_LEN; i++)
    message[i] = i % 256;
  write_be32 (message, FPC_DISUM_IMAGE_CODE);
  write_be32 (message + 4, FPC_DISUM_IMAGE_TOTAL_LEN);
  write_be32 (message + 8, 0);

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  collector.expected_message = message;
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_APPLICATION, collect_frame, &collector);

  g_assert_true (fpc_disum_parser_feed (&parser, message, FPC_DISUM_IMAGE_TOTAL_LEN));
  g_assert_true (collector.got_image);
  g_assert_true (collector.pixels_checked);

  g_ptr_array_unref (collector.frames);
}

static void
test_feed_length_limit (void)
{
  Collector collector = { 0 };
  g_auto (FpcDisumParser) parser = { 0 };
  const guint8 byte = 0;

  collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
  fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_EVENT, collect_frame, &collector);

  /*
   * A read the buffer cannot represent must fail explicitly.  Narrowing it to its
   * low 32 bits would drop data and still report success.
   */
  g_assert_false (fpc_disum_parser_feed (&parser, &byte, (gsize)G_MAXUINT + 1));
  g_assert_cmpstr (fpc_disum_parser_error (&parser), !=, NULL);
  g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, 0);
  g_assert_cmpuint (collector.frames->len, ==, 0);

  g_ptr_array_unref (collector.frames);
}

static void
test_frame_length_bounds (void)
{
  static const struct
  {
    guint32 length;
    gboolean accepted;
    gsize pending;
    guint frames;
  } cases[] = {
    /* rejected at the length probe: every fed byte is kept for diagnostics */
    { FPC_DISUM_FRAME_MIN_LEN - 1, FALSE, FPC_DISUM_FRAME_HEADER_LEN, 0 },
    /* the smallest complete frame is delivered and consumed */
    { FPC_DISUM_FRAME_MIN_LEN, TRUE, 0, 1 },
    /* a valid but large frame stays incomplete after the header alone */
    { FPC_DISUM_FRAME_MAX_LEN, TRUE, FPC_DISUM_FRAME_HEADER_LEN, 0 },
    { FPC_DISUM_FRAME_MAX_LEN + 1, FALSE, FPC_DISUM_FRAME_HEADER_LEN, 0 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      Collector collector = { 0 };
      g_auto (FpcDisumParser) parser = { 0 };
      guint8 header[FPC_DISUM_FRAME_HEADER_LEN] = { 0 };
      gboolean accepted;

      write_be32 (header, 6);
      write_be32 (header + 4, cases[i].length);

      collector.frames = g_ptr_array_new_with_free_func (observed_frame_free);
      fpc_disum_parser_init (&parser, FPC_DISUM_STREAM_EVENT, collect_frame, &collector);

      accepted = fpc_disum_parser_feed (&parser, header, sizeof (header));
      g_assert_cmpint (accepted, ==, cases[i].accepted);
      g_assert_cmpint (fpc_disum_parser_error (&parser) == NULL, ==, cases[i].accepted);
      g_assert_cmpuint (fpc_disum_parser_pending (&parser), ==, cases[i].pending);
      g_assert_cmpuint (collector.frames->len, ==, cases[i].frames);

      g_ptr_array_unref (collector.frames);
    }
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/protocol/partial-frame-retained", test_partial_frame_retained);
  g_test_add_func ("/protocol/invalid-length-reported-early", test_invalid_length_reported_early);
  g_test_add_func ("/protocol/feed-length-limit", test_feed_length_limit);
  g_test_add_func (
      "/protocol/stopped-parser-ignores-further-data", test_stopped_parser_ignores_further_data);
  g_test_add_func (
      "/protocol/capture-accepts-only-image-frames", test_capture_accepts_only_image_frames);
  g_test_add_func ("/protocol/image-plane-at-declared-offset", test_image_plane_at_declared_offset);
  g_test_add_func ("/protocol/frame-length-bounds", test_frame_length_bounds);

  for (gsize i = 0; i < fpc_disum_corpus_n_event_vectors; i++)
    {
      g_autofree gchar *path
          = g_strdup_printf ("/protocol/vector/event/%s", fpc_disum_corpus_event_vectors[i].id);
      g_test_add_data_func (path, &fpc_disum_corpus_event_vectors[i], test_event_vector);
    }

  for (gsize i = 0; i < fpc_disum_corpus_n_application_vectors; i++)
    {
      g_autofree gchar *path = g_strdup_printf (
          "/protocol/vector/application/%s", fpc_disum_corpus_application_vectors[i].id);
      g_test_add_data_func (
          path, &fpc_disum_corpus_application_vectors[i], test_application_vector);
    }

  return g_test_run ();
}
