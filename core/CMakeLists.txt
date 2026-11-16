add_library(nexus_core
  src/engine.cpp
  src/engine_journal.cpp
  src/mft_scanner.cpp
  src/scanner.cpp
  src/search.cpp
  src/snapshot.cpp
  src/usn_record_parser.cpp
  src/usn_journal_reader.cpp
  src/volume.cpp
)
target_include_directories(nexus_core PUBLIC include)
target_compile_definitions(nexus_core PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(nexus_core PRIVATE /W4 /permissive- /utf-8)
else()
  message(FATAL_ERROR "NEXUS Search Phase 1 currently targets Windows and MSVC.")
endif()
