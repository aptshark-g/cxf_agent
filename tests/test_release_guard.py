#!/usr/bin/env python3
"""Publication checks: sensitive values fail without being returned in findings."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release_guard", ROOT / "tools/prepare_github_release.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class ReleaseGuardTest(unittest.TestCase):
    def rules(self, text):
        return {finding["rule"] for finding in guard.scan_text("fixture", text)}

    def test_tokens_and_redaction(self):
        for prefix in ("sk-", "ghp_", "github_pat_", "Bearer "):
            value = prefix + "Q" * 40
            findings = guard.scan_text("fixture", "safe\n" + value)
            self.assertTrue(findings)
            self.assertEqual(findings[0]["line"], 2)
            self.assertNotIn(value, repr(findings))

    def test_json_credentials(self):
        for name in ("api_key", "key", "password", "access_token"):
            value = '{"' + name + '": "' + "synthetic" + '"}'
            self.assertIn("nonempty_key_assignment", self.rules(value))
        self.assertFalse(self.rules('{"key": ""}'))
        self.assertFalse(self.rules("key = 'weighted_'"))

    def test_url_and_home_boundaries(self):
        private = "http://" + "192.168.50.9:8000/v1/chat/completions"
        self.assertIn("private_or_embedded_service_url", self.rules(private))
        service = "https://" + "service.example.net/v1/chat/completions"
        self.assertIn("private_or_embedded_service_url", self.rules(service))
        unversioned = "https://" + "service.example.net/chat/completions"
        self.assertIn("private_or_embedded_service_url", self.rules(unversioned))
        self.assertNotIn(unversioned, repr(guard.scan_text("fixture", unversioned)))
        self.assertFalse(self.rules('{"model":"deepseek-v4-flash-0731"}'))
        credentials = "https://" + "user:synthetic@example.com/resource"
        self.assertIn("url_credentials", self.rules(credentials))
        home = "/".join(("", "Users", "synthetic", "models", "file"))
        self.assertIn("home_path", self.rules(home))
        for safe in ("http://127.0.0.1:8000/v1/chat/completions",
                     "http://localhost:8080/v1/chat/completions",
                     "https://github.com/aptshark-g/cxf_agent",
                     "http://example.com/v1/chat/completions"):
            self.assertFalse(self.rules(safe))

    def test_symlink_is_not_exported(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "docs").mkdir()
            (root / "docs/sensitive.md").symlink_to(root / "outside.md")
            with self.assertRaisesRegex(ValueError, "symlink rejected"):
                guard.public_files(root)

    def test_compressed_image_metadata_is_scanned(self):
        secret = "sk-" + "S" * 40
        payload = b"Comment\0\0" + zlib.compress(secret.encode())
        kind = b"zTXt"
        data = b"\x89PNG\r\n\x1a\n" + struct.pack(">I", len(payload)) + kind + payload
        data += struct.pack(">I", zlib.crc32(kind + payload))
        self.assertTrue(any(self.rules(text) for text in guard.png_metadata(data)))
        with self.assertRaisesRegex(ValueError, "truncated PNG"):
            guard.png_metadata(data[:-1])


if __name__ == "__main__":
    unittest.main()
