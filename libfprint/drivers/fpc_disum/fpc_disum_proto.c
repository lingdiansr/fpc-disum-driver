/*
 * FPC Disum (10a5:9201) frame and application-message parsing.
 *
 * Reference: docs/CLEANROOM_SPEC.md §4.4 (frames), §5 (image), §16.1 (failure).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_disum_proto.h"

/* Frames are big-endian on the wire; do not use the host-endian conversion
 * macros. */
static guint32
read_be32 (const guint8 *data)
{
  return ((guint32)data[0] << 24) | ((guint32)data[1] << 16) | ((guint32)data[2] << 8)
         | ((guint32)data[3]);
}

static const gchar *
stream_subject (FpcDisumStream stream)
{
  /*
   * The wording of the event-layer error is fixed by the reviewed vectors
   * (`ProtocolError: invalid event length <len>`). The application layer uses
   * the same shape as an engineering convention; no vector pins its wording.
   */
  return stream == FPC_DISUM_STREAM_APPLICATION ? "message" : "event";
}

void
fpc_disum_parser_init (
    FpcDisumParser *parser, FpcDisumStream stream, FpcDisumFrameFunc callback, gpointer user_data)
{
  g_return_if_fail (parser != NULL);

  fpc_disum_parser_clear (parser);

  parser->buffer = g_byte_array_new ();
  parser->error = NULL;
  parser->stream = stream;
  parser->callback = callback;
  parser->user_data = user_data;
}

void
fpc_disum_parser_clear (FpcDisumParser *parser)
{
  if (parser == NULL)
    return;

  g_clear_pointer (&parser->buffer, g_byte_array_unref);
  g_clear_pointer (&parser->error, g_free);
  parser->stream = FPC_DISUM_STREAM_EVENT;
  parser->callback = NULL;
  parser->user_data = NULL;
}

gboolean
fpc_disum_parser_feed (FpcDisumParser *parser, const guint8 *data, gsize len)
{
  g_return_val_if_fail (parser != NULL, FALSE);

  /* Parsing already stopped: the retained bytes are diagnostics and must not
   * move. */
  if (parser->error != NULL)
    return FALSE;

  /*
   * GByteArray stores its length in a guint.  A read that cannot be represented
   * must fail explicitly rather than be narrowed to its low 32 bits, which would
   * silently drop data and still report success (SPEC §16.1).
   */
  if (len > (gsize)G_MAXUINT - parser->buffer->len)
    {
      parser->error = g_strdup ("ParserError: input length exceeds the buffer limit");
      return FALSE;
    }

  if (len > 0)
    {
      g_return_val_if_fail (data != NULL, FALSE);
      g_byte_array_append (parser->buffer, data, len);
    }

  while (parser->buffer->len >= FPC_DISUM_FRAME_LENGTH_PROBE_LEN)
    {
      guint32 total_len;
      FpcDisumFrame frame;

      total_len = read_be32 (parser->buffer->data + FPC_DISUM_FRAME_LENGTH_OFFSET);

      if (total_len < FPC_DISUM_FRAME_MIN_LEN || total_len > FPC_DISUM_FRAME_MAX_LEN)
        {
          parser->error = g_strdup_printf (
              "ProtocolError: invalid %s length %u", stream_subject (parser->stream), total_len);
          return FALSE;
        }

      /* Hold incomplete data; a short read is not a frame boundary. */
      if (parser->buffer->len < total_len)
        break;

      frame.code = read_be32 (parser->buffer->data);
      frame.total_len = total_len;
      frame.third_field = read_be32 (parser->buffer->data + 8);
      frame.payload_len = total_len - FPC_DISUM_FRAME_HEADER_LEN;
      frame.payload = parser->buffer->data + FPC_DISUM_FRAME_HEADER_LEN;

      if (parser->callback != NULL)
        parser->callback (&frame, parser->user_data);

      g_byte_array_remove_range (parser->buffer, 0, total_len);
    }

  return TRUE;
}

gsize
fpc_disum_parser_pending (const FpcDisumParser *parser)
{
  g_return_val_if_fail (parser != NULL, 0);

  return parser->buffer != NULL ? parser->buffer->len : 0;
}

const gchar *
fpc_disum_parser_error (const FpcDisumParser *parser)
{
  g_return_val_if_fail (parser != NULL, NULL);

  return parser->error;
}

gboolean
fpc_disum_frame_is_image (const FpcDisumFrame *frame)
{
  g_return_val_if_fail (frame != NULL, FALSE);

  return frame->code == FPC_DISUM_IMAGE_CODE && frame->total_len == FPC_DISUM_IMAGE_TOTAL_LEN;
}

gboolean
fpc_disum_frame_get_image (const FpcDisumFrame *frame, FpcDisumImage *image)
{
  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (image != NULL, FALSE);

  if (!fpc_disum_frame_is_image (frame))
    return FALSE;

  /* The 24 B prefix (message header plus inner header) must close exactly. */
  if ((gsize)FPC_DISUM_IMAGE_PIXEL_OFFSET + FPC_DISUM_IMAGE_PIXELS != (gsize)frame->total_len)
    return FALSE;

  image->width = FPC_DISUM_IMAGE_WIDTH;
  image->height = FPC_DISUM_IMAGE_HEIGHT;
  image->stride = FPC_DISUM_IMAGE_STRIDE;
  image->pixel_len = FPC_DISUM_IMAGE_PIXELS;
  image->pixels = frame->payload + (FPC_DISUM_IMAGE_PIXEL_OFFSET - FPC_DISUM_FRAME_HEADER_LEN);

  return TRUE;
}
