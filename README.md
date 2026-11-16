# NEXUS Search

NEXUS Search is an experimental Windows file-search application with a C++20 indexing and search engine, a PySide6 desktop interface, and a pybind11 bridge. It is a work in progress; several Windows filesystem features are implemented only partially and have not been validated against a real, unrestricted volume.

## Features

- Exact, prefix, and substring filename search in a native in-memory index.
- Case-insensitive matching, extension/path/drive/type/size/date filters, and a 100-result UI limit.
- Background recursive indexing with per-entry error reporting and a best-effort NTFS MFT enumeration path.
- Versioned binary snapshots with CRC validation and atomic replacement.
- Experimental USN Journal batch reading and limited create/delete/metadata updates when a stable MFT identity map is available.
- PySide6 desktop app and a self-contained Windows executable build.

The desktop app starts indexing `C:\` on each launch and displays the active folder above the search box. Full-volume indexing may take time and use substantial memory. You can choose a different folder after the current scan finishes. The app does not automatically index other drives.

## Requirements

- Windows 11 x64
- Visual Studio 2022 C++ tools (MSVC)
- CMake 3.20 or newer
- Python 3.10 or newer
- Python packages from `requirements.txt`
- Network access on the first CMake configure if GoogleTest is not already available

## Build and test

Open a **Developer PowerShell for Visual Studio 2022** at the repository root:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
$pybind11Dir = python -m pybind11 --cmakedir
cmake -S . -B build -A x64 "-Dpybind11_DIR=$pybind11Dir" -DNEXUS_FETCH_GTEST=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
$extension = Get-ChildItem .\build\bindings -Recurse -Filter "_nexus*.pyd" | Select-Object -First 1
$env:PYTHONPATH = $extension.DirectoryName
python -m unittest discover -s tests\python -v
```

The C++ engine targets Windows/MSVC. GoogleTest is fetched by CMake if it is not installed. To skip the Python binding or tests, configure with `-DNEXUS_BUILD_PYTHON=OFF` or `-DNEXUS_BUILD_TESTS=OFF` respectively.

## Run

After building, launch the GUI:

```powershell
python gui\qt_main.py
```

It begins scanning `C:\` automatically. Search matches filenames and folder names; use the path filter to narrow results by location. Indexing errors appear in the status line. The command-line smoke interface is also available:

```powershell
$env:PYTHONPATH = $extension.DirectoryName
python gui\main.py C:\Some\Folder prog
```

## Build the Windows executable

```powershell
python -m pip install -r requirements-build.txt
.\packaging\build_windows.ps1
```

This creates a self-contained `NEXUS Search.exe` at the repository root. Generated executables and PyInstaller files are excluded from Git; publish the executable as a GitHub Release asset if you distribute it.

## Known limitations

- MFT enumeration and live USN reads have not been verified on an unrestricted real volume in the development environment. They may fall back or return access denied.
- USN rename, move, directory, and hard-link cases request a full reconciliation. The desktop app does not yet run journal recovery automatically.
- Snapshots can be saved and loaded manually; startup snapshot recovery is not connected to the GUI.
- Search filters are available, but fuzzy search and configurable shortcuts are not implemented.
- Paths are stored per record; compact path/string storage and memory-mapped snapshots are not implemented.
- Benchmarks use synthetic empty-file datasets. The largest captured dataset has 1.5 million files; a five-million-file run and controlled comparisons with Everything or Windows Search are outstanding. Results are documented in [docs/performance.md](docs/performance.md).

## Project documentation

- [Architecture and current limits](docs/architecture.md)
- [NTFS MFT notes](docs/ntfs_mft.md)
- [GUI guide](docs/gui.md)
- [Performance measurements](docs/performance.md)
- [Development notes](docs/development.md)

## License

NEXUS Search is distributed under the MIT License. See [LICENSE](LICENSE).
