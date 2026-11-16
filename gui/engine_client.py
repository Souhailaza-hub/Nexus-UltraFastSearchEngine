"""Small asynchronous client for the NEXUS C++ engine."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import _nexus


class EngineClient:
    def __init__(self):
        self.engine = _nexus.Engine()
        self._pool = ThreadPoolExecutor(max_workers=2, thread_name_prefix="nexus-search")

    def search_async(self, query: str, mode: str = "prefix", limit: int = 100, filters=None):
        native_filter = _nexus.SearchFilter()
        for name, value in (filters or {}).items():
            setattr(native_filter, name, value)
        return self._pool.submit(self.engine.search_filtered, query, mode, native_filter, limit)

    def index_async(self, path: str | Path):
        return self._pool.submit(self.engine.index_path, str(path))

    def stats_async(self):
        return self._pool.submit(self.engine.stats)

    def save_snapshot_async(self, path: str | Path):
        return self._pool.submit(self.engine.save_snapshot, str(path))

    def load_snapshot_async(self, path: str | Path):
        return self._pool.submit(self.engine.load_snapshot, str(path))

    def close(self):
        self.engine.stop_indexing()
        self._pool.shutdown(wait=True, cancel_futures=True)
