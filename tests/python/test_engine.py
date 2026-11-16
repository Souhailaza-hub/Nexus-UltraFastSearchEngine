"""Smoke tests for the compiled NEXUS Search extension; run with python -m unittest."""
import unittest

try:
    import _nexus
except ImportError:
    _nexus = None


@unittest.skipIf(_nexus is None, "Build the _nexus extension before running Python tests")
class EngineSmokeTest(unittest.TestCase):
    def test_index_exact_and_prefix(self):
        import tempfile
        from pathlib import Path

        with tempfile.TemporaryDirectory(prefix="nexus-test-") as tmp:
            root = Path(tmp)
            nested = root / "nested"
            nested.mkdir()
            (root / "alpha.txt").write_text("a", encoding="utf-8")
            (root / "alphabet.py").write_text("b", encoding="utf-8")
            (nested / "progress.txt").write_text("c", encoding="utf-8")
            (root / "telegram.txt").write_text("d", encoding="utf-8")
            engine = _nexus.Engine()
            engine.index_path(str(root) + "\\")
            self.assertEqual([r.name for r in engine.search_exact("ALPHA.TXT")], ["alpha.txt"])
            self.assertEqual({r.name for r in engine.search_prefix("alph")}, {"alpha.txt", "alphabet.py"})
            self.assertEqual({r.name for r in engine.search_substring("BET")}, {"alphabet.py"})
            nested_record = engine.search_exact("nested")[0]
            progress = engine.search_exact("progress.txt")[0]
            self.assertEqual(progress.parent_id, nested_record.file_id)
            self.assertTrue(Path(progress.path).is_absolute())

    def test_journal_reader_rejects_invalid_root(self):
        result = _nexus.read_usn_journal("invalid", 0, 0)
        self.assertEqual(result.state, _nexus.JournalReadState.Error)
        checkpoint = _nexus.query_usn_journal_checkpoint("invalid")
        self.assertEqual(checkpoint.state, _nexus.JournalReadState.Error)

    def test_sync_requires_mft_backed_index(self):
        result = _nexus.Engine().sync_usn_journal("C:\\", 0, 0)
        self.assertEqual(result.state, _nexus.JournalReadState.ReconciliationRequired)
        self.assertFalse(result.applied)

    def test_snapshot_round_trip(self):
        import tempfile
        from pathlib import Path

        with tempfile.TemporaryDirectory(prefix="nexus-snapshot-") as tmp:
            root = Path(tmp)
            (root / "resume.txt").write_text("snapshot", encoding="utf-8")
            engine = _nexus.Engine()
            engine.index_path(str(root))
            snapshot = root / "index.nxs"
            self.assertTrue(engine.save_snapshot(str(snapshot)))
            restored = _nexus.Engine()
            self.assertTrue(restored.load_snapshot(str(snapshot)))
            self.assertEqual([r.name for r in restored.search_exact("resume.txt")], ["resume.txt"])

    def test_search_filters_are_applied_before_the_result_limit(self):
        import tempfile
        from pathlib import Path

        with tempfile.TemporaryDirectory(prefix="nexus-filter-") as tmp:
            root = Path(tmp)
            nested = root / "nested"
            nested.mkdir()
            (root / "alpha-small.txt").write_text("x", encoding="utf-8")
            (nested / "alpha-large.TXT").write_text("enough bytes", encoding="utf-8")
            (nested / "alpha-large.bin").write_text("enough bytes", encoding="utf-8")
            engine = _nexus.Engine()
            engine.index_path(str(root))
            filters = _nexus.SearchFilter()
            filters.extension = ".txt"
            filters.path_prefix = str(nested)
            filters.files_only = True
            filters.min_size_bytes = 5
            result = engine.search_filtered("alpha", "prefix", filters, 1)
            self.assertEqual([record.name for record in result], ["alpha-large.TXT"])


if __name__ == "__main__":
    unittest.main()
