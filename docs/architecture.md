# Architecture and current limits

`nexus_core` is a Python-independent C++20 library. `Engine` owns the records and in-memory name indexes: an exact-name hash, an ordered name map for prefix ranges, a trigram posting index, and one-/two-character substring postings. Substring candidates are checked against cached normalized names. Search is case-insensitive and returns original names. `FileRecord` currently retains a full path per entry; path interning and parent-based reconstruction are future memory work.

## Discovery and indexing

For NTFS volume roots, the scanner attempts `FSCTL_ENUM_USN_DATA`, parses bounds-checked V2 records, resolves parent references, and constructs paths before publishing records. A denied volume handle, malformed page, or unresolved parent triggers recursive-scan fallback and increments `mft_fallbacks`. Non-NTFS roots and subdirectory scans use recursive enumeration. The MFT implementation queries file size and modification time per reconstructed path; it has not been measured on a real volume because the current environment denied the volume handle.

Recursive scanning enumerates one directory at a time and commits batches of up to 1,024 records under a mutex. Permission failures in one child directory increment the error count while sibling directories continue to be scanned. Directory reparse points are not followed, preventing junction loops and unintended traversal outside the selected tree. Search copies bounded result sets while holding that mutex, so it sees a consistent published batch as indexing continues. Native worker scans can be stopped and joined; scan-control operations are serialized. Metadata and traversal failures increment the error count.

## Change journal

The USN reader can query the journal and read one bounded `FSCTL_READ_USN_JOURNAL` batch. `Engine.SyncUsnJournal` applies supported file creation, deletion, and data/metadata changes only when the volume has a stable MFT-backed identity map. Duplicate file IDs are coalesced; deleted entries are tombstoned and compacted after they exceed one quarter of an index larger than 4,096 records. Rename, directory, hard-link, missing-parent, stale-journal, and unknown-ID cases return `ReconciliationRequired` without advancing the checkpoint.

Recovery should save the checkpoint before a full MFT reconciliation, rebuild the volume, then replay from the saved journal ID and USN. Repeat reconciliation if the journal wraps during the scan. A checkpoint is committed only when a batch reports `applied`. The current machine denied access to C: for journal reads, so live volume updates have only synthetic test coverage.

## Snapshots

Snapshots contain a versioned binary header, active file records, MFT volume identities, and committed USN checkpoints. A CRC-32 protects the payload. Saves write a sibling temporary file and atomically replace the destination. Loads validate sizes, versions, strings, counts, and checksum before replacing current state; corrupt input leaves the live index unchanged. Search indexes are reconstructed on load. Memory-mapped loading has not been justified by measurements and is not implemented.

## Search filters

The engine applies optional extension, path-prefix, volume, file/folder, size, and modification-time constraints to exact, prefix, and substring candidate streams before applying the result limit. Size ranges exclude directories. The GUI expresses modification dates as UTC days and validates size/date text before submission.

## Desktop app and packaging

The PySide6 window debounces search, dispatches indexing/search/snapshot calls through a worker pool, discards obsolete search responses, and supports open/reveal/copy actions. The app is user-driven: it indexes a selected folder rather than automatically crawling every drive. Windows global Ctrl+Space is attempted, with local shortcuts remaining available if registration fails. A PyInstaller one-folder release build is produced by `packaging/build_windows.ps1`; the script replaces an incompatible ICU DLL PyInstaller may pick from the build environment with Windows' ICU DLL. Its `--smoke` mode permits headless startup validation.

## Remaining engineering work

- Implement safe rename and directory-move journal handling; real journal access is required to validate it.
- Integrate startup snapshot loading, volume validation, and journal reconciliation in the app.
- Add search filters and configurable shortcuts.
- Reduce per-record path and string duplication, then measure allocation and cache behavior.
- Run five-million-file and controlled Everything/Windows Search comparisons on a machine with sufficient RAM.
- Profile the MFT route on a volume where the process has the required handle access.
