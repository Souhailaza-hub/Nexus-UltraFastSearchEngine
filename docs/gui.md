# PySide6 interface

Launch the desktop UI with `python gui/qt_main.py` after setting `PYTHONPATH` to the CMake `_nexus` module output directory. It provides a debounced search box, prefix/substring/exact modes, and filters for extension, path prefix, drive, files/folders, minimum/maximum size, and modification dates. Size fields accept B, KB, MB, GB, or TB; date fields use UTC `YYYY-MM-DD`. Filters are applied inside the native search so the result limit is applied after filtering. The 100-result table shows duration and metadata, with background folder indexing, file open/reveal/copy actions, and snapshot load/save.

Keyboard controls: Ctrl+Space shows and focuses NEXUS (registered as a Windows global hotkey when available), Escape hides the window, Enter opens the selected result, Ctrl+Enter reveals it in Explorer, Ctrl+C copies its path, Ctrl+Backspace clears the query, and Up/Down navigate results.

The app starts indexing `C:\` automatically and shows the active index path above the search field. The folder button can select a different folder after the current scan finishes. The application does not automatically enumerate other mounted drives. Search and indexing run outside Qt's main thread; obsolete search responses are discarded by generation number after a newer query is submitted.

For a self-contained Windows executable, install `requirements-build.txt`, build the native extension, and run `packaging/build_windows.ps1`. It writes `NEXUS Search.exe` beside the repository's top-level files. Set `QT_QPA_PLATFORM=offscreen` and pass `--smoke` for a headless launch check.
