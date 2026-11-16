cmake_minimum_required(VERSION 3.20)
project(NexusSearch VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

option(NEXUS_BUILD_PYTHON "Build the pybind11 module" ON)
option(NEXUS_BUILD_TESTS "Build C++ tests" ON)
option(NEXUS_FETCH_GTEST "Fetch GoogleTest when it is not already installed" ON)
option(NEXUS_BUILD_BENCHMARKS "Build the native indexing/search benchmark" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(core)

if(NEXUS_BUILD_PYTHON)
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 CONFIG QUIET)
  if(pybind11_FOUND)
    add_subdirectory(bindings)
  else()
    message(WARNING "pybind11 was not found; building the C++ engine only. Install pybind11 and reconfigure to build the Python module.")
  endif()
endif()

if(NEXUS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/cpp)
endif()

if(NEXUS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
