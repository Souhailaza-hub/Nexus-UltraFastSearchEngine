# Development

Use a Visual Studio Developer PowerShell and build with CMake as described in the root README. Keep filesystem-specific work in `core/src/scanner.cpp` and `core/src/volume.cpp`; preserve the Python-independent `nexus_core` boundary. The Python API is intentionally small and should not expose index internals.

When changing the scanner or search indexes, run CTest and Python unittest after configuring the corresponding dependencies. For large scale work, add benchmark datasets and report hardware, filesystem, data volume, warm/cold state, p50/p95/p99 latency, indexing time, and memory. Do not extrapolate performance targets from small smoke tests.
