# Dependency resolution.
#
# Policy: prefer whatever the machine already has (system packages, vcpkg,
# Conan, a CMAKE_PREFIX_PATH the developer set up). Only reach out to the
# network for the header-only libraries that are awkward to install and cheap
# to vendor. SQLite is deliberately *not* fetched: it is the one real native
# dependency, every platform packages it, and pinning our own copy of a
# database engine is a maintenance liability.

include(FetchContent)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- SQLite3 -----------------------------------------------------------------
# Ubuntu/Debian: apt install libsqlite3-dev
# macOS:         brew install sqlite   (then -DCMAKE_PREFIX_PATH=$(brew --prefix sqlite))
# Windows:       vcpkg install sqlite3
find_package(SQLite3 REQUIRED)
message(STATUS "Using SQLite3 ${SQLite3_VERSION} (${SQLite3_LIBRARIES})")

# --- nlohmann/json -----------------------------------------------------------
find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found locally - fetching v3.11.3")
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- cpp-httplib -------------------------------------------------------------
# Header-only, blocking, thread-per-connection. Chosen for readability over
# raw throughput; see README "Trade-offs".
find_package(httplib QUIET)
if(NOT httplib_FOUND)
  message(STATUS "httplib not found locally - fetching v0.18.7")
  FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.7
    GIT_SHALLOW    TRUE)
  set(HTTPLIB_COMPILE OFF CACHE INTERNAL "")
  set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE INTERNAL "")
  set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(httplib)
endif()

# --- doctest (tests only) ----------------------------------------------------
# Chosen over Catch2 purely for build time: it is one header and compiles in
# seconds, which matters when the test binary is rebuilt on every change.
#
# SOURCE_SUBDIR points at a directory that does not exist on purpose. That
# downloads the sources without configuring doctest's own CMake project, whose
# `cmake_minimum_required(VERSION 3.0)` is rejected outright by CMake 4.x. We
# only want the header, so we declare the interface target ourselves rather
# than carrying a policy override for a dependency we barely use.
if(URLSHORT_BUILD_TESTS)
  find_package(doctest QUIET)
  if(NOT doctest_FOUND)
    message(STATUS "doctest not found locally - fetching v2.4.11")
    FetchContent_Declare(doctest
      GIT_REPOSITORY https://github.com/doctest/doctest.git
      GIT_TAG        v2.4.11
      GIT_SHALLOW    TRUE
      SOURCE_SUBDIR  cmake-project-intentionally-not-used)
    FetchContent_MakeAvailable(doctest)

    add_library(urlshort_doctest INTERFACE)
    target_include_directories(urlshort_doctest INTERFACE "${doctest_SOURCE_DIR}")
    add_library(doctest::doctest ALIAS urlshort_doctest)
  endif()
endif()
