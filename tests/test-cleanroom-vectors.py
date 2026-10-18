#!/usr/bin/env python3
"""Regression checks for the clean-room vector verifier itself.

The verifier is the tool that substantiates the claim that the reviewed vectors can
be rebuilt from their declarative description.  These cases make sure it fails loudly
instead of passing vacuously when an input or an expectation is tampered with.
"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "fpc_disum_vectors.py"
VECTORS = ROOT / "docs" / "CLEANROOM_TEST_VECTORS.json"


class CleanroomVectorToolTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="fpc-disum-vectors-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "vectors.json"
        self.document = json.loads(VECTORS.read_text(encoding="utf-8"))

    def run_tool(self):
        self.path.write_text(json.dumps(self.document), encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(TOOL), "--vectors", str(self.path), "--verify"],
            capture_output=True, text=True, check=False)

    def vector(self, vector_id):
        return next(v for v in self.document["vectors"] if v["id"] == vector_id)

    def test_accepts_reviewed_vectors(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("22 vectors verified", result.stdout)

    def test_rejects_tampered_pixel_expectation(self):
        self.vector("pixels/ridge-accepted")["expect"]["row_difference"] += 0.001
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_tampered_frame_payload_hash(self):
        self.vector("outer-frame/min-length-12")["expect"]["frames"][0]["payload_sha256"] = "0" * 64
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_missing_vector(self):
        self.document["vectors"].pop()
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_empty_vector_set(self):
        self.document["vectors"] = []
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_missing_pixel_expectation(self):
        self.vector("pixels/ridge-accepted")["expect"].pop("row_difference")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_tampered_envelope_expectation(self):
        self.vector("tls-envelope/121-valid")["expect"]["signature_offset"] = 95
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_accepted_vector_without_psk(self):
        self.vector("tls-envelope/121-valid")["expect"].pop("psk_b64")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_unreachable_envelope_stage(self):
        self.vector("tls-envelope/bad-hmac-rejected")["expect"]["reject_stage"] = "field-bounds"
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_unknown_vector_kind(self):
        self.document["vectors"][0]["kind"] = "unknown_kind"
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_rejects_non_synthetic_document(self):
        self.document["synthetic"] = False
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout)


if __name__ == "__main__":
    unittest.main()
