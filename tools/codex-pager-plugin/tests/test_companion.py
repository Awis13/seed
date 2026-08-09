from pathlib import Path
import subprocess
import tempfile
import unittest
from urllib.parse import parse_qs, urlparse


THREAD_ID = "01900000-0000-7000-8000-000000000000"
SOURCE = Path(__file__).parents[1] / "companion" / "CodexPagerCompanion.swift"


class CompanionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.executable = Path(cls.directory.name) / "codex-pager-companion"
        subprocess.run(
            [
                "swiftc",
                "-warnings-as-errors",
                "-framework",
                "AppKit",
                "-framework",
                "ApplicationServices",
                str(SOURCE),
                "-o",
                str(cls.executable),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_url_round_trips_unicode_and_reserved_characters(self):
        prompt = "こんにちは 🚀 & #1?\nnext"
        result = subprocess.run(
            [
                str(self.executable),
                "url",
                "--thread",
                THREAD_ID,
                "--prompt",
                prompt,
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        parsed = urlparse(result.stdout.strip())
        self.assertEqual(parsed.scheme, "codex")
        self.assertEqual(parsed.netloc, "threads")
        self.assertEqual(parsed.path, f"/{THREAD_ID}")
        self.assertEqual(parse_qs(parsed.query)["prompt"], [prompt])

    def test_invalid_task_id_fails_without_opening_accessibility(self):
        result = subprocess.run(
            [
                str(self.executable),
                "url",
                "--thread",
                "newest",
                "--prompt",
                "hello",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Invalid Codex task id", result.stderr)

    def test_task_proof_uses_window_attributes_not_descendants(self):
        source = SOURCE.read_text(encoding="utf-8")
        proof = source.split("func provesTask", 1)[1].split("func hasAction", 1)[0]
        self.assertIn("kAXTitleAttribute", proof)
        self.assertIn("kAXDocumentAttribute", proof)
        self.assertNotIn("descendants(", proof)

    def test_button_fallback_is_bounded_to_direct_container_children(self):
        source = SOURCE.read_text(encoding="utf-8")
        selection = source.split("func associatedSubmitButton", 1)[1].split(
            "func submit", 1
        )[0]
        self.assertIn("for _ in 0..<2", selection)
        self.assertIn("children(container)", selection)
        self.assertNotIn("descendants(", selection)


if __name__ == "__main__":
    unittest.main()
