#!/usr/bin/env python3
"""Exercise reviewed-baseline acceptance and tampering rejection without USB."""

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "tools" / "check-cleanroom.py"


class CleanroomBaselineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="fpc-disum-baseline-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "docs").mkdir()
        for name in ("CLEANROOM_SPEC.md", "CLEANROOM_TEST_VECTORS.json", "SHA256SUMS"):
            shutil.copyfile(ROOT / "docs" / name, self.root / "docs" / name)

    def check_baseline(self):
        return subprocess.run(
            [sys.executable, str(CHECKER), "--root", str(self.root)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_accepts_reviewed_baseline(self):
        result = self.check_baseline()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_modified_specification(self):
        with (self.root / "docs" / "CLEANROOM_SPEC.md").open("ab") as stream:
            stream.write(b"\n")
        result = self.check_baseline()
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_rejects_repinning_manifest_with_modified_specification(self):
        specification = self.root / "docs" / "CLEANROOM_SPEC.md"
        with specification.open("rb") as stream:
            original = hashlib.file_digest(stream, "sha256").hexdigest()
        with specification.open("ab") as stream:
            stream.write(b"\n")
        with specification.open("rb") as stream:
            modified = hashlib.file_digest(stream, "sha256").hexdigest()
        manifest = self.root / "docs" / "SHA256SUMS"
        manifest.write_text(manifest.read_text().replace(original, modified))
        result = self.check_baseline()
        self.assertEqual(result.returncode, 1, result.stderr)


if __name__ == "__main__":
    unittest.main()
