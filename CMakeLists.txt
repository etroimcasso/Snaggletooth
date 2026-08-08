cmake_minimum_required(VERSION 3.24)

project(Snaggletooth VERSION 0.0.1 LANGUAGES CXX)

# A build is optimized by default: an empty CMAKE_BUILD_TYPE (CMake's default when nothing
# supplies one) would produce an unoptimized, unstripped library. Multi-config generators
# ignore this and select the configuration at build time.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Route compilation through ccache when it is installed, so warm rebuilds on a
# persistent runner (or a developer's machine) are fast. A no-op when absent.
if(PROJECT_IS_TOP_LEVEL)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  endif()
endif()

# On by default when Snaggletooth is the top-level project; off when it is vendored into a
# parent build (the parent runs its own suite). Override with -DSNAGGLETOOTH_BUILD_TESTS=ON/OFF.
option(SNAGGLETOOTH_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})

add_library(snaggletooth STATIC
  src/version.cpp
  src/apu.cpp
  src/dsp.cpp
)
add_library(snaggletooth::snaggletooth ALIAS snaggletooth)

target_compile_features(snaggletooth PUBLIC cxx_std_20)
set_target_properties(snaggletooth PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(snaggletooth PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Warnings are errors on Snaggletooth's own targets only — never imposed on a parent build or
# on fetched dependencies.
if(MSVC)
  target_compile_options(snaggletooth PRIVATE /W4 /WX)
else()
  target_compile_options(snaggletooth PRIVATE -Wall -Wextra -Werror)
endif()

if(SNAGGLETOOTH_BUILD_TESTS)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  # Keep GoogleTest on the same CRT as the library on Windows.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # Path to the SingleStepTests SPC700 "v1" directory (per-opcode JSON vectors).
  # When empty, the SPC700 vector tests register but skip with a visible reason.
  # The vectors are large, machine-generated reference data — never vendored.
  set(SNAGGLETOOTH_SPC700_VECTORS "" CACHE PATH
      "Path to the SingleStepTests SPC700 'v1' vector directory")

  add_executable(snaggletooth_tests
    tests/version_test.cpp
    tests/spc700/vector_harness.cpp
    tests/spc700/harness_test.cpp
    tests/spc700/algorithm_test.cpp
    tests/spc700/vector_test.cpp
    tests/spc700/dummy_read_test.cpp
    tests/apu/machine_test.cpp
    tests/apu/port_test.cpp
    tests/apu/timer_test.cpp
    tests/dsp/brr_test.cpp
    tests/dsp/dsp_state_test.cpp
    tests/dsp/pitch_gauss_test.cpp
    tests/dsp/envelope_test.cpp
    tests/dsp/voice_mix_test.cpp
  )
  target_link_libraries(snaggletooth_tests PRIVATE
    snaggletooth::snaggletooth
    GTest::gtest_main
  )
  target_compile_definitions(snaggletooth_tests PRIVATE
    SNAGGLETOOTH_SPC700_VECTORS="${SNAGGLETOOTH_SPC700_VECTORS}")
  if(MSVC)
    target_compile_options(snaggletooth_tests PRIVATE /W4 /WX)
    # std::getenv is flagged C4996 ("unsafe") by MSVC; with /WX that stops the
    # build. It reads a well-known variable name here, so accept the standard CRT
    # function rather than the _s variant.
    target_compile_definitions(snaggletooth_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(snaggletooth_tests PRIVATE -Wall -Wextra -Werror)
  endif()

  include(GoogleTest)
  gtest_discover_tests(snaggletooth_tests)
endif()
