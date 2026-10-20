#!/usr/bin/env python3
"""Verify the exact reviewed protocol inputs, independent of the local manifest."""

import argparse
import hashlib
from pathlib import Path
import sys


EXPECTED_DIGESTS = {
    "docs/CLEANROOM_SPEC.md": "b8feda4fc944d41c78347d5d08927ae86bcec38da35002360666c6bf20387b00",
    "docs/CLEANROOM_TEST_VECTORS.json": "ce04ce43665f2b2d27142f2c2ecd917b09219a87a82d4b3009c023baecfeda94",
}


def verify(root: Path) -> None:
    manifest = {}
    for line in (root / "docs" / "SHA256SUMS").read_text().splitlines():
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 2:
            raise ValueError("malformed docs/SHA256SUMS entry")
        digest, name = fields
        if name in manifest:
            raise ValueError(f"duplicate manifest entry: {name}")
        manifest[name] = digest
    if manifest != EXPECTED_DIGESTS:
        raise ValueError("manifest does not match the reviewed baseline")

    for name, expected in EXPECTED_DIGESTS.items():
        with (root / name).open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            raise ValueError(f"digest mismatch: {name}")
        print(f"OK {name} {actual}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1],
        help="project root containing the reviewed docs/ inputs",
    )
    args = parser.parse_args()
    try:
        verify(args.root)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"cleanroom verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
