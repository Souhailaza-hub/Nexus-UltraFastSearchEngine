# Performance results

## Local synthetic datasets

| Property | Result |
|---|---:|
| CPU | Intel Core i5-13450HX, 10 cores / 16 logical processors |
| RAM | 15.7 GiB |
| OS | Windows 11 build 26200 |
| Filesystem | NTFS |
| Dataset | 100,000 empty files in one temporary directory |
| Scanner path | Recursive `std::filesystem` scan; MFT is only attempted for a volume-root scan |
| File generation | 16.409 s, excluded from indexing time |
| Index time | 3.748 s |
| Index estimate | 130,057,786 bytes (about 1.30 KiB per file) |
| Process working set | 145,620,992 bytes after indexing |
| Index errors | 0 |
| Search sample | 200 warm native C++ queries per search type |

| Search | p50 | p95 | p99 |
|---|---:|---:|---:|
| Exact | 0.40 µs | 0.50 µs | 0.80 µs |
| Prefix | 14.90 µs | 18.90 µs | 20.40 µs |
| Substring | 14.60 µs | 15.30 µs | 17.50 µs |

| Dataset | 500,000 empty files in one temporary directory |
| Scanner path | Recursive `std::filesystem` scan |
| File generation | 97.377 s, excluded from indexing time |
| Index time | 17.845 s |
| Index estimate | 666,421,114 bytes (about 1.33 KiB per file) |
| Process working set | 702,464,000 bytes after indexing |
| Index errors | 0 |
| Search sample | 100 warm native C++ queries per search type |

| Search | p50 | p95 | p99 |
|---|---:|---:|---:|
| Exact | 0.40 µs | 0.50 µs | 2.10 µs |
| Prefix | 19.20 µs | 23.00 µs | 68.90 µs |
| Substring | 18.60 µs | 19.50 µs | 22.40 µs |

| Dataset | 1,000,000 empty files in one temporary directory |
| Scanner path | Recursive `std::filesystem` scan |
| File generation | 710.633 s, excluded from indexing time |
| Index time | 30.407 s |
| Index estimate | 1,168,362,666 bytes (about 1.17 KiB per file) |
| Process working set | 1,402,241,024 bytes after indexing |
| Index errors | 0 |
| Search sample | 100 warm native C++ queries per search type |

| Search | p50 | p95 | p99 |
|---|---:|---:|---:|
| Exact | 0.30 µs | 0.40 µs | 0.60 µs |
| Prefix | 14.40 µs | 16.30 µs | 18.60 µs |
| Substring | 14.60 µs | 15.20 µs | 18.20 µs |

| Dataset | 1,500,000 empty files across 512 subdirectories |
| Scanner path | Recursive `std::filesystem` scan |
| File generation | 405.141 s, excluded from indexing time |
| Index time | 76.448 s |
| Index estimate | 1,757,707,326 bytes (about 1.17 KiB per file) |
| Process working set | 2,808,750,080 bytes after indexing |
| Index errors | 0 |
| Search sample | 50 warm native C++ queries per search type |

| Search | p50 | p95 | p99 |
|---|---:|---:|---:|
| Exact | 0.30 µs | 0.40 µs | 0.50 µs |
| Prefix | 20.50 µs | 91.50 µs | 153.40 µs |
| Substring | 27.20 µs | 58.90 µs | 85.10 µs |

These are single local runs, not a comparison against Everything or Windows Search. The generated datasets contain empty files and do not represent a real user volume. Sharding avoids one huge directory, but 1.5 million files still took 6.75 minutes to generate and 76 seconds to index. The index estimate excludes allocator and index-node overhead, while the process working set includes more than the index. Five million files were not run: based on this run, expected memory exceeds the machine's 4.7 GiB free RAM at start, so that test was deferred to avoid excessive paging. MFT performance and controlled competitor comparisons remain unmeasured.

Run `build/benchmarks/nexus_benchmark.exe --files 100000 --runs 200` to reproduce a flat dataset. Pass `--dirs 1024` to shard files across directories for larger scales, for example `build/benchmarks/nexus_benchmark.exe --files 5000000 --dirs 1024 --runs 100`. The benchmark reports generation separately, validates file count and errors, reports p50/p95/p99, and removes its temporary dataset afterward. It flushes results before cleanup so long deletion times do not delay the report.
