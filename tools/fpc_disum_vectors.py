#!/usr/bin/env python3
"""Reconstruct and verify the reviewed clean-room vectors; emit the C test corpus.

The only device-protocol input for this project is the reviewed delivery package
(docs/CLEANROOM_SPEC.md and docs/CLEANROOM_TEST_VECTORS.json).  This tool rebuilds
every vector from its declarative description (spec appendix B), checks the
recorded expectations against that reconstruction, and emits the subset the native
C tests consume.  It never needs a device, a key, an envelope, or a captured image.

Exit status: 0 verified, 1 verification failure, 2 unusable input.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import random
import struct
import sys
from pathlib import Path

FRAME_HEADER_LEN = 12
FRAME_LENGTH_OFFSET = 4
FRAME_LENGTH_PROBE_LEN = 8
FRAME_MIN_LEN = 12
FRAME_MAX_LEN = 65536

IMAGE_CODE = 8
IMAGE_TOTAL_LEN = 9880
IMAGE_WIDTH = 112
IMAGE_HEIGHT = 88
IMAGE_STRIDE = 112
IMAGE_PIXEL_OFFSET = 24
IMAGE_PIXELS = IMAGE_WIDTH * IMAGE_HEIGHT

ENVELOPE_LEN = 121
ENVELOPE_MAGIC = 0x0DEC0DED
ENVELOPE_SIGNATURE_LEN = 32

ACCEPT_THRESHOLD = 0.85

CORPUS_BASE = "fpc-disum-corpus"
CORPUS_KINDS = {"event_stream", "application_stream", "tls_envelope"}
VERIFIED_KINDS = CORPUS_KINDS | {"pixels", "tls_envelope"}

# The reviewed package is a fixed baseline: a truncated or extended vector set must
# not be reported as verified, even when every remaining vector still passes.
EXPECTED_VECTOR_IDS = (
    "outer-frame/split-across-64b-packets",
    "outer-frame/two-frames-in-one-packet",
    "outer-frame/empty-packet-inside-frame",
    "outer-frame/short-packets-reassemble",
    "outer-frame/timeout-keeps-partial",
    "outer-frame/min-length-12",
    "outer-frame/max-length-65536",
    "outer-frame/length-11-rejected",
    "outer-frame/length-65537-rejected",
    "outer-frame/unknown-code-nonzero-third",
    "application-message/image-frame",
    "application-message/partial-held-until-complete",
    "pixels/horizontal-ramp-accepted",
    "pixels/vertical-ramp-accepted",
    "pixels/ridge-accepted",
    "pixels/noise-rejected",
    "pixels/zeros-rejected",
    "tls-envelope/121-valid",
    "tls-envelope/length-72-rejected",
    "tls-envelope/bad-hmac-rejected",
    "tls-envelope/hardcoded-key-offset-rejected",
    "tls-envelope/signature-offset-out-of-range-rejected",
)

PIXEL_EXPECTED_FIELDS = (
    "accepted",
    "column_difference",
    "horizontal_correlation",
    "row_difference",
    "vertical_correlation",
)

ENVELOPE_EXPECTED_FIELDS = (
    "key_offset",
    "key_length",
    "aad_offset",
    "aad_length",
    "signature_offset",
    "signature_length",
)


class VectorError(Exception):
    """A vector could not be reconstructed or its expectation did not hold."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VectorError(message)


# --------------------------------------------------------------------------- #
# Declarative input reconstruction (spec appendix B.3 "patterns")              #
# --------------------------------------------------------------------------- #


def build_pattern(spec: dict) -> bytes:
    kind = spec["kind"]

    if kind == "literal":
        return bytes.fromhex(spec["hex"])
    if kind == "counter":
        return bytes(i % 256 for i in range(spec["length"]))
    if kind == "inner_header_image":
        return bytes.fromhex(spec["header_hex"]) + build_pattern(spec["image"])
    if kind == "zeros":
        return bytes(IMAGE_PIXELS)
    if kind == "noise":
        return random.Random(spec["seed"]).randbytes(IMAGE_PIXELS)
    if kind == "horizontal_ramp":
        return bytes((x * spec["step"]) % 256
                     for y in range(IMAGE_HEIGHT) for x in range(IMAGE_WIDTH))
    if kind == "vertical_ramp":
        return bytes((y * spec["step"]) % 256
                     for y in range(IMAGE_HEIGHT) for x in range(IMAGE_WIDTH))
    if kind == "ridge":
        period = spec["period"]

        def sample(x: int, y: int) -> int:
            value = (128.0
                     + 60.0 * math.sin(2 * math.pi * x / period + 0.9 * math.sin(2 * math.pi * y / 30))
                     + 25.0 * math.sin(2 * math.pi * y / 17))
            # appendix B.3: evaluate, clamp to [0, 255], then truncate towards zero.
            return int(max(0.0, min(255.0, value)))

        return bytes(sample(x, y) for y in range(IMAGE_HEIGHT) for x in range(IMAGE_WIDTH))

    raise VectorError(f"unknown pattern kind: {kind!r}")


def frame_header(code: int, payload_len: int, third_field: int) -> bytes:
    return struct.pack(">III", code, FRAME_HEADER_LEN + payload_len, third_field)


def build_event_wire(vector: dict) -> bytes:
    if "raw" in vector:
        return build_pattern(vector["raw"])
    return b"".join(frame_header(frame["code"], len(build_pattern(frame["payload"])), frame["unknown"])
                    + build_pattern(frame["payload"])
                    for frame in vector["frames"])


def build_application_wire(vector: dict) -> bytes:
    payload = build_pattern(vector["inner"])
    return frame_header(vector["outer_code"], len(payload), vector["outer_unknown"]) + payload


# --------------------------------------------------------------------------- #
# Independent reconstruction of the framing expectations                       #
# --------------------------------------------------------------------------- #


def consume_frames(buffer: bytearray, subject: str) -> dict:
    """Extract every complete frame from ``buffer``, consuming it in place.

    The length is validated as soon as eight header bytes are available, so an
    invalid length is reported without waiting for the full header.
    """
    frames: list[dict] = []

    while len(buffer) >= FRAME_LENGTH_PROBE_LEN:
        total_len = int.from_bytes(buffer[FRAME_LENGTH_OFFSET:FRAME_LENGTH_OFFSET + 4], "big")
        if total_len < FRAME_MIN_LEN or total_len > FRAME_MAX_LEN:
            return {"frames": frames, "error": f"ProtocolError: invalid {subject} length {total_len}"}
        if len(buffer) < total_len:
            break

        payload = bytes(buffer[FRAME_HEADER_LEN:total_len])
        frames.append({
            "code": int.from_bytes(buffer[0:4], "big"),
            "total_len": total_len,
            "unknown": int.from_bytes(buffer[8:12], "big"),
            "payload_len": total_len - FRAME_HEADER_LEN,
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
        })
        del buffer[:total_len]

    return {"frames": frames, "error": None}


def simulate_stream(wire: bytes, plan: list, subject: str) -> dict:
    """Replay a read plan over the wire and rebuild the framing expectations.

    ``plan`` entries are read sizes, or ``"timeout"`` for a read that returned
    nothing.  A read never returns more bytes than remain in the wire.
    """
    buffer = bytearray()
    frames: list[dict] = []
    offset = 0
    timeouts = 0

    for chunk in plan:
        if chunk == "timeout":
            timeouts += 1
            continue

        available = min(chunk, len(wire) - offset)
        buffer.extend(wire[offset:offset + available])
        offset += available

        step = consume_frames(buffer, subject)
        frames.extend(step["frames"])
        if step["error"] is not None:
            return {"frames": frames, "error": step["error"],
                    "held_partial_bytes": len(buffer), "timeouts": timeouts}

    timeouts += 1  # the final read of a successful stream also times out

    return {"frames": frames, "error": None, "held_partial_bytes": len(buffer), "timeouts": timeouts}


def verify_event_vector(vector: dict) -> None:
    wire = build_event_wire(vector)
    require(sum(c for c in vector["chunks"] if c != "timeout") <= len(wire),
            f"{vector['id']}: read plan exceeds the reconstructed wire")
    rebuilt = simulate_stream(wire, vector["chunks"], "event")
    expected = vector["expect"]

    require(rebuilt["error"] == expected["error"],
            f"{vector['id']}: error {rebuilt['error']!r} != declared {expected['error']!r}")
    require(rebuilt["held_partial_bytes"] == expected["held_partial_bytes"],
            f"{vector['id']}: held {rebuilt['held_partial_bytes']} != declared {expected['held_partial_bytes']}")
    require(rebuilt["timeouts"] == expected["timeouts"],
            f"{vector['id']}: timeouts {rebuilt['timeouts']} != declared {expected['timeouts']}")

    declared = [{k: f[k] for k in ("code", "total_len", "unknown", "payload_len", "payload_sha256")}
                for f in expected["frames"]]
    require(rebuilt["frames"] == declared,
            f"{vector['id']}: frames {rebuilt['frames']} != declared {declared}")

    for frame in expected["frames"]:
        require(frame["total_len"] == FRAME_HEADER_LEN + frame["payload_len"],
                f"{vector['id']}: declared frame does not close (total_len != 12 + payload_len)")


def verify_application_vector(vector: dict) -> None:
    wire = build_application_wire(vector)
    layout = vector.get("layout")

    if layout is not None:
        require(wire[:FRAME_HEADER_LEN].hex() == layout["outer_header_hex"],
                f"{vector['id']}: outer header {wire[:FRAME_HEADER_LEN].hex()} != declared {layout['outer_header_hex']}")
        payload = wire[FRAME_HEADER_LEN:]
        require(payload[:FRAME_HEADER_LEN].hex() == layout["inner_header_hex"],
                f"{vector['id']}: inner header {payload[:12].hex()} != declared {layout['inner_header_hex']}")
        require(layout["pixel_offset"] + layout["pixels"] == len(wire),
                f"{vector['id']}: pixel_offset + pixels != total length")
        require(layout["pixels"] == IMAGE_PIXELS and layout["stride"] == IMAGE_STRIDE,
                f"{vector['id']}: declared pixel geometry is not 112x88/stride 112")

    rebuilt = simulate_stream(wire, vector["feeds"], "message")
    expected = vector["expect"]

    require(rebuilt["error"] == expected["error"],
            f"{vector['id']}: error {rebuilt['error']!r} != declared {expected['error']!r}")

    declared = [{"code": m["code"], "total_len": m["total_len"], "payload_len": m["payload_len"]}
                for m in expected["messages"]]
    rebuilt_messages = [{"code": m["code"], "total_len": m["total_len"], "payload_len": m["payload_len"]}
                        for m in rebuilt["frames"]]
    require(rebuilt_messages == declared,
            f"{vector['id']}: messages {rebuilt_messages} != declared {declared}")

    # Rebuild the per-feed state the vector records for the first read.
    buffer = bytearray(wire[:min(vector["feeds"][0], len(wire))])
    first = consume_frames(buffer, "message")
    consumed = sum(message["total_len"] for message in first["frames"])
    pending = len(buffer)

    require(consumed == expected["consumed_after_first_feed"],
            f"{vector['id']}: consumed {consumed} != declared {expected['consumed_after_first_feed']}")
    require(pending == expected["pending_after_first_feed"],
            f"{vector['id']}: pending {pending} != declared {expected['pending_after_first_feed']}")

    for message in expected["messages"]:
        require(message["total_len"] == FRAME_HEADER_LEN + message["payload_len"],
                f"{vector['id']}: declared message does not close")


# --------------------------------------------------------------------------- #
# Pixel metrics (spec appendix B.1)                                            #
# --------------------------------------------------------------------------- #


def pearson(xs: list, ys: list) -> float:
    n = len(xs)
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    cov = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    var_x = sum((x - mean_x) ** 2 for x in xs)
    var_y = sum((y - mean_y) ** 2 for y in ys)
    if var_x == 0.0 or var_y == 0.0:
        return 0.0
    return cov / math.sqrt(var_x * var_y)


def pixel_metrics(pixels: bytes) -> dict:
    require(len(pixels) == IMAGE_PIXELS, f"pixel plane is {len(pixels)} bytes, expected {IMAGE_PIXELS}")
    rows = [pixels[y * IMAGE_WIDTH:(y + 1) * IMAGE_WIDTH] for y in range(IMAGE_HEIGHT)]

    # Correlations compare the overlapping area only; the edges are not wrapped.
    left = [rows[y][x] for y in range(IMAGE_HEIGHT) for x in range(IMAGE_WIDTH - 1)]
    right = [rows[y][x + 1] for y in range(IMAGE_HEIGHT) for x in range(IMAGE_WIDTH - 1)]
    top = [rows[y][x] for y in range(IMAGE_HEIGHT - 1) for x in range(IMAGE_WIDTH)]
    bottom = [rows[y + 1][x] for y in range(IMAGE_HEIGHT - 1) for x in range(IMAGE_WIDTH)]

    horizontal = pearson(left, right)
    vertical = pearson(top, bottom)

    return {
        "stddev": math.sqrt(sum((p - sum(pixels) / len(pixels)) ** 2 for p in pixels) / len(pixels)),
        "row_difference": sum(abs(a - b) for a, b in zip(left, right)) / len(left),
        "column_difference": sum(abs(a - b) for a, b in zip(top, bottom)) / len(top),
        "horizontal_correlation": horizontal,
        "vertical_correlation": vertical,
        "accepted": max(horizontal, vertical) >= ACCEPT_THRESHOLD,
    }


def verify_pixel_vector(vector: dict) -> None:
    expected = vector["expect"]
    missing = [key for key in PIXEL_EXPECTED_FIELDS if key not in expected]
    require(not missing, f"{vector['id']}: pixel vector does not declare {missing}")

    metrics = pixel_metrics(build_pattern(vector["pixels"]))
    for key in PIXEL_EXPECTED_FIELDS:
        if key == "accepted":
            require(metrics["accepted"] == expected[key],
                    f"{vector['id']}: accepted {metrics['accepted']} != declared {expected[key]}")
        else:
            require(round(metrics[key], 3) == expected[key],
                    f"{vector['id']}: {key} {round(metrics[key], 3)} != declared {expected[key]}")


# --------------------------------------------------------------------------- #
# Envelope structure (spec §8 / appendix B.3-B.4)                              #
# --------------------------------------------------------------------------- #


def envelope_structural_stage(raw: bytes) -> str | None:
    """First failure stage that can be decided without AES or HMAC.

    The check order is the one the spec fixes: length/magic, then field bounds
    (including key length being a multiple of 16 and the signature being 32 bytes),
    then the cryptographic stages which this tool does not implement.
    """
    if len(raw) != ENVELOPE_LEN:
        return "total-length"
    magic = int.from_bytes(raw[0:4], "little")
    if magic != ENVELOPE_MAGIC:
        return "total-length"

    _, key_offset, key_length, aad_offset, aad_length, sig_offset, sig_length = struct.unpack_from("<7I", raw, 0)
    if key_length % 16 != 0 or sig_length != ENVELOPE_SIGNATURE_LEN:
        return "field-bounds"
    for offset, length in ((key_offset, key_length), (aad_offset, aad_length), (sig_offset, sig_length)):
        if offset > len(raw) or length > len(raw) - offset:
            return "field-bounds"
    return None


def verify_envelope_vector(vector: dict) -> None:
    raw = build_pattern(vector["envelope"])
    expected = vector["expect"]
    stage = envelope_structural_stage(raw)

    if stage is not None:
        require("error" in expected,
                f"{vector['id']}: structurally rejected ({stage}) but declared as accepted")
        declared_stage = expected.get("reject_stage")
        if declared_stage is not None:
            require(declared_stage == stage,
                    f"{vector['id']}: structural stage {stage} != declared {declared_stage}")
    else:
        declared_stage = expected.get("reject_stage")
        require(declared_stage not in ("total-length", "field-bounds"),
                f"{vector['id']}: declared {declared_stage} but the structure is in range")

    if "error" not in expected:
        # An accepted vector must record, and match, every header field it relies on.
        require("psk_b64" in expected,
                f"{vector['id']}: accepted vector does not declare psk_b64")
        require(len(raw) == ENVELOPE_LEN, f"{vector['id']}: accepted vector is not {ENVELOPE_LEN} bytes")
        _, key_offset, key_length, aad_offset, aad_length, sig_offset, sig_length = \
            struct.unpack_from("<7I", raw, 0)
        declared = {
            "key_offset": key_offset,
            "key_length": key_length,
            "aad_offset": aad_offset,
            "aad_length": aad_length,
            "signature_offset": sig_offset,
            "signature_length": sig_length,
        }
        for key in ENVELOPE_EXPECTED_FIELDS:
            require(key in expected, f"{vector['id']}: accepted vector does not declare {key}")
            require(expected[key] == declared[key],
                    f"{vector['id']}: {key} {declared[key]} != declared {expected[key]}")

    if "psk_b64" in expected:
        psk = base64.b64decode(expected["psk_b64"])
        require(len(psk) == 32, f"{vector['id']}: declared PSK is not 32 bytes")
        _, _, key_length, *_ = struct.unpack_from("<7I", raw, 0)
        require(key_length == len(psk),
                f"{vector['id']}: key length {key_length} != declared PSK length {len(psk)}")


# --------------------------------------------------------------------------- #
# Verification entry point                                                     #
# --------------------------------------------------------------------------- #


def load_vectors(path: Path) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise VectorError(f"cannot read {path}: {error}") from error

    require(document.get("synthetic") is True, "vector file is not marked synthetic")

    seen = set()
    for vector in document["vectors"]:
        require(vector["kind"] in VERIFIED_KINDS,
                f"{vector['id']}: unknown vector kind {vector['kind']!r}")
        require(vector["id"] not in seen, f"duplicate vector id {vector['id']}")
        seen.add(vector["id"])

    expected = set(EXPECTED_VECTOR_IDS)
    missing = sorted(expected - seen)
    unexpected = sorted(seen - expected)
    require(not missing and not unexpected,
            "vector set does not match the reviewed package: "
            f"missing={missing} unexpected={unexpected}")
    return document


def verify_vectors(document: dict) -> dict:
    handlers = {
        "event_stream": verify_event_vector,
        "application_stream": verify_application_vector,
        "pixels": verify_pixel_vector,
        "tls_envelope": verify_envelope_vector,
    }
    counts: dict[str, int] = {}
    for vector in document["vectors"]:
        handlers[vector["kind"]](vector)
        counts[vector["kind"]] = counts.get(vector["kind"], 0) + 1
    return counts


# --------------------------------------------------------------------------- #
# C corpus emission                                                            #
# --------------------------------------------------------------------------- #


def render_blob(name: str, data: bytes) -> str:
    lines = [f"static const guint8 {name}[] = {{"]
    for index in range(0, len(data), 16):
        lines.append("  " + " ".join(f"0x{byte:02x}," for byte in data[index:index + 16]))
    lines.append("};")
    return "\n".join(lines)


def emit_corpus(document: dict, directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)

    blobs: list[str] = []
    event_entries: list[str] = []
    application_entries: list[str] = []
    envelope_entries: list[str] = []

    for index, vector in enumerate(document["vectors"]):
        kind = vector["kind"]
        if kind not in CORPUS_KINDS:
            continue

        expected = vector["expect"]
        prefix = f"v{index}"

        if kind == "tls_envelope":
            envelope = build_pattern(vector["envelope"])
            blob = f"envelope_{index}"
            blobs.append(render_blob(blob, envelope))
            accepted = "error" not in expected
            stage = expected.get("reject_stage")
            stage_ref = "NULL" if stage is None else '"%s"' % stage
            psk = base64.b64decode(expected["psk_b64"]) if accepted else b""
            if psk:
                blobs.append(render_blob(f"psk_{index}", psk))
                psk_ref = f"psk_{index}"
            else:
                psk_ref = "NULL"
            envelope_entries.append(
                '  { "%s", %s, %d, %s, %s, %d, %s }' % (
                    vector["id"], blob, len(envelope),
                    "TRUE" if accepted else "FALSE", stage_ref, len(psk), psk_ref))
            continue

        wire = build_event_wire(vector) if kind == "event_stream" else build_application_wire(vector)
        blob = f"wire_{index}"
        blobs.append(render_blob(blob, wire))

        if kind == "event_stream":
            chunks = ", ".join(
                "{ TRUE, 0 }" if chunk == "timeout" else f"{{ FALSE, {chunk} }}"
                for chunk in vector["chunks"])
            blobs.append(f"static const FpcDisumCorpusChunk chunks_{prefix}[] = {{ {chunks} }};")

            if expected["frames"]:
                frames = ",\n  ".join(
                    '{ %d, %d, %d, %d, "%s" }' % (frame["code"], frame["total_len"], frame["unknown"],
                                                  frame["payload_len"], frame["payload_sha256"])
                    for frame in expected["frames"])
                frames_ref = f"frames_{prefix}"
                blobs.append(f"static const FpcDisumCorpusFrame {frames_ref}[] = {{\n  {frames}\n}};")
            else:
                frames_ref = "NULL"

            error = 'NULL' if expected["error"] is None else '"%s"' % expected["error"]
            event_entries.append(
                '  { "%s", %s, %d, chunks_%s, %d, %s, %d, %s, %d, %d }' % (
                    vector["id"], blob, len(wire), prefix, len(vector["chunks"]),
                    frames_ref, len(expected["frames"]), error,
                    expected["held_partial_bytes"], expected["timeouts"]))
        else:
            feeds = ", ".join(str(feed) for feed in vector["feeds"])
            blobs.append(f"static const gsize feeds_{prefix}[] = {{ {feeds} }};")

            messages = ",\n  ".join(
                "{ %d, %d, %d }" % (message["code"], message["total_len"], message["payload_len"])
                for message in expected["messages"])
            blobs.append(f"static const FpcDisumCorpusMessage messages_{prefix}[] = {{\n  {messages}\n}};")

            layout = vector.get("layout")
            has_layout = "TRUE" if layout is not None else "FALSE"
            pixel_offset = layout["pixel_offset"] if layout is not None else 0
            pixels = layout["pixels"] if layout is not None else 0
            stride = layout["stride"] if layout is not None else 0
            accepted = expected["messages"][0]["code"] == IMAGE_CODE and \
                expected["messages"][0]["total_len"] == IMAGE_TOTAL_LEN

            application_entries.append(
                '  { "%s", %s, %d, feeds_%s, %d, messages_%s, %d, %d, %d, %s, %s, %d, %d, %d }' % (
                    vector["id"], blob, len(wire), prefix, len(vector["feeds"]),
                    prefix, len(expected["messages"]),
                    expected["pending_after_first_feed"],
                    expected["consumed_after_first_feed"],
                    "TRUE" if accepted else "FALSE",
                    has_layout, pixel_offset, pixels, stride))

    header = f'''/* Generated from docs/CLEANROOM_TEST_VECTORS.json by tools/fpc_disum_vectors.py.
 * Do not edit; the build system regenerates it. */
#pragma once

#include <glib.h>

typedef struct {{
  guint32 code;
  guint32 total_len;
  guint32 third_field;
  gsize payload_len;
  const gchar *payload_sha256;
}} FpcDisumCorpusFrame;

typedef struct {{
  gboolean is_timeout;
  gsize bytes;
}} FpcDisumCorpusChunk;

typedef struct {{
  const gchar *id;
  const guint8 *wire;
  gsize wire_len;
  const FpcDisumCorpusChunk *chunks;
  gsize n_chunks;
  const FpcDisumCorpusFrame *frames;
  gsize n_frames;
  const gchar *error;
  gsize held_partial_bytes;
  gsize timeouts;
}} FpcDisumCorpusEventVector;

typedef struct {{
  guint32 code;
  guint32 total_len;
  gsize payload_len;
}} FpcDisumCorpusMessage;

typedef struct {{
  const gchar *id;
  const guint8 *wire;
  gsize wire_len;
  const gsize *feeds;
  gsize n_feeds;
  const FpcDisumCorpusMessage *messages;
  gsize n_messages;
  gsize pending_after_first_feed;
  gsize consumed_after_first_feed;
  gboolean accepted_as_image;
  gboolean has_layout;
  gsize pixel_offset;
  gsize pixels;
  gsize stride;
}} FpcDisumCorpusApplicationVector;

typedef struct {{
  const gchar *id;
  const guint8 *envelope;
  gsize envelope_len;
  gboolean accepted;
  const gchar *reject_stage;
  gsize psk_len;
  const guint8 *psk;
}} FpcDisumCorpusEnvelopeVector;

extern const FpcDisumCorpusEventVector fpc_disum_corpus_event_vectors[];
extern const gsize fpc_disum_corpus_n_event_vectors;
extern const FpcDisumCorpusApplicationVector fpc_disum_corpus_application_vectors[];
extern const gsize fpc_disum_corpus_n_application_vectors;
extern const FpcDisumCorpusEnvelopeVector fpc_disum_corpus_envelope_vectors[];
extern const gsize fpc_disum_corpus_n_envelope_vectors;
'''

    source = "\n".join([
        "/* Generated from docs/CLEANROOM_TEST_VECTORS.json by tools/fpc_disum_vectors.py.",
        " * Do not edit; the build system regenerates it. */",
        f'#include "{CORPUS_BASE}.h"',
        "",
        "\n\n".join(blobs),
        "",
        "const FpcDisumCorpusEventVector fpc_disum_corpus_event_vectors[] = {",
        ",\n".join(event_entries),
        "};",
        "const gsize fpc_disum_corpus_n_event_vectors = G_N_ELEMENTS (fpc_disum_corpus_event_vectors);",
        "",
        "const FpcDisumCorpusApplicationVector fpc_disum_corpus_application_vectors[] = {",
        ",\n".join(application_entries),
        "};",
        "const gsize fpc_disum_corpus_n_application_vectors = G_N_ELEMENTS (fpc_disum_corpus_application_vectors);",
        "",
        "const FpcDisumCorpusEnvelopeVector fpc_disum_corpus_envelope_vectors[] = {",
        ",\n".join(envelope_entries),
        "};",
        "const gsize fpc_disum_corpus_n_envelope_vectors = G_N_ELEMENTS (fpc_disum_corpus_envelope_vectors);",
        "",
    ])

    (directory / f"{CORPUS_BASE}.h").write_text(header, encoding="utf-8")
    (directory / f"{CORPUS_BASE}.c").write_text(source, encoding="utf-8")


# --------------------------------------------------------------------------- #
# Command line                                                                 #
# --------------------------------------------------------------------------- #


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectors", type=Path,
                        default=Path(__file__).resolve().parents[1] / "docs" / "CLEANROOM_TEST_VECTORS.json",
                        help="reviewed vector file to reconstruct")
    parser.add_argument("--verify", action="store_true", help="reconstruct and check every vector")
    parser.add_argument("--emit-corpus", type=Path, metavar="DIR",
                        help="write the generated C corpus into DIR")
    options = parser.parse_args()

    if not options.verify and options.emit_corpus is None:
        options.verify = True

    try:
        document = load_vectors(options.vectors)
        counts = verify_vectors(document)
        if options.emit_corpus is not None:
            emit_corpus(document, options.emit_corpus)
    except VectorError as error:
        print(f"cleanroom vector verification failed: {error}", file=sys.stderr)
        return 1

    if options.verify:
        for kind in sorted(counts):
            print(f"OK {kind}: {counts[kind]} vectors reconstructed from the declarative description")
        print(f"OK total: {sum(counts.values())} vectors verified against their expectations")
        if counts.get("tls_envelope"):
            print("NOTE tls_envelope: structure verified here; AES/HMAC acceptance is verified "
                  "by the native envelope tests (M2)")
    if options.emit_corpus is not None:
        print(f"OK corpus written to {options.emit_corpus}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
