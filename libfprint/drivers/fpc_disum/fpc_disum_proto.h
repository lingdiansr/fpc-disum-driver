/*
 * FPC Disum (10a5:9201) frame and application-message parsing.
 *
 * Reference: docs/CLEANROOM_SPEC.md §4.4 (frames), §5 (image), §16.1 (failure).
 *
 * This file is part of libfprint.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * L2 outer event frames and L3 application messages share a 12 B header:
 *
 *   code (u32 BE) + length (u32 BE, including the header) + third field (u32
 * BE)
 *
 * The length is validated as soon as eight header bytes are available, before
 * the full header is buffered, so a malformed length is reported immediately
 * and the received bytes are kept for diagnostics (SPEC §4.4/§16.1).
 */
#define FPC_DISUM_FRAME_HEADER_LEN 12
#define FPC_DISUM_FRAME_LENGTH_OFFSET 4
#define FPC_DISUM_FRAME_LENGTH_PROBE_LEN 8
#define FPC_DISUM_FRAME_MIN_LEN 12
#define FPC_DISUM_FRAME_MAX_LEN 65536

/*
 * Application image message (SPEC §5): a 12 B message header, a 12 B inner
 * header, then a 112x88 8-bit greyscale plane in row-major order with no row
 * padding. The coordinate system is a specification convention: the origin is
 * the first byte at offset 24, x increases along the row, y increases along the
 * column, and the plane must never be rotated, transposed or flipped.
 */
#define FPC_DISUM_IMAGE_CODE 8
#define FPC_DISUM_IMAGE_TOTAL_LEN 9880
#define FPC_DISUM_IMAGE_WIDTH 112
#define FPC_DISUM_IMAGE_HEIGHT 88
#define FPC_DISUM_IMAGE_STRIDE 112
#define FPC_DISUM_IMAGE_PIXELS (FPC_DISUM_IMAGE_WIDTH * FPC_DISUM_IMAGE_HEIGHT)
#define FPC_DISUM_IMAGE_PIXEL_OFFSET 24

typedef enum
{
  /* L2 outer event frames carried on the bulk IN endpoint (SPEC §4.4). */
  FPC_DISUM_STREAM_EVENT,
  /* L3 application messages after TLS decryption (SPEC §5). */
  FPC_DISUM_STREAM_APPLICATION,
} FpcDisumStream;

typedef struct
{
  guint32 code;
  guint32 total_len;     /* including the 12 B header */
  guint32 third_field;   /* preserved verbatim, never interpreted */
  gsize payload_len;     /* total_len - FPC_DISUM_FRAME_HEADER_LEN */
  const guint8 *payload; /* borrowed; only valid during the callback */
} FpcDisumFrame;

typedef void (*FpcDisumFrameFunc) (const FpcDisumFrame *frame, gpointer user_data);

typedef struct
{
  GByteArray *buffer;
  gchar *error;
  FpcDisumStream stream;
  FpcDisumFrameFunc callback;
  gpointer user_data;
} FpcDisumParser;

/*
 * Initialise a parser.  The parser must be zero-initialised (or already cleared);
 * init is idempotent and releases a previous buffer.  Frames are delivered from
 * inside fpc_disum_parser_feed(); the payload view points into the parser buffer
 * and is only valid until the callback returns.
 */
void fpc_disum_parser_init (
    FpcDisumParser *parser, FpcDisumStream stream, FpcDisumFrameFunc callback, gpointer user_data);
void fpc_disum_parser_clear (FpcDisumParser *parser);
/*
 * Feed bytes read from the endpoint.  A complete frame is delivered through the
 * callback; a malformed length stops parsing and keeps every unconsumed byte.
 * Once stopped, further feeds are refused and the retained bytes stay untouched.
 * A read larger than the buffer can represent is refused rather than narrowed.
 */
gboolean fpc_disum_parser_feed (FpcDisumParser *parser, const guint8 *data, gsize len);
gsize fpc_disum_parser_pending (const FpcDisumParser *parser);
const gchar *fpc_disum_parser_error (const FpcDisumParser *parser);

typedef struct
{
  guint32 width;
  guint32 height;
  guint32 stride;
  gsize pixel_len;
  const guint8 *pixels; /* borrowed from the frame */
} FpcDisumImage;

/* Whether the capture layer may treat this message as an image (SPEC §5). */
gboolean fpc_disum_frame_is_image (const FpcDisumFrame *frame);
/* Locate the pixel plane; fails unless the frame is an image and closes
 * exactly. */
gboolean fpc_disum_frame_get_image (const FpcDisumFrame *frame, FpcDisumImage *image);

G_END_DECLS
