# Benchmarks

The native benchmark creates empty-file datasets, indexes them through the recursive scanner, validates counts and errors, and reports generation time separately from indexing and warm exact/prefix/substring latency. `--dirs N` distributes files round-robin across N subdirectories; the default of one directory preserves the original flat-dataset shape. Sharding avoids making one very large NTFS directory the only generator bottleneck and allows multi-million-file tests to finish in a practical time.

Example: `build/benchmarks/nexus_benchmark.exe --files 5000000 --dirs 1024 --runs 100`. Benchmark results remain specific to the machine and generated dataset; compare them only when the scanner path, filesystem, warm/cold state, and query samples match.
