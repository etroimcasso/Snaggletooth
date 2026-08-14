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

# The .spc tooling — the loader, the WAV writer, and the spc_render command-line
# renderer. A small static helper carries the loader and writer so the CLI and the
# tests both link them. Built for a top-level developer build or whenever the tests
# are on; a parent build that vendors the library and runs its own suite pulls in
# none of it.
if(PROJECT_IS_TOP_LEVEL OR SNAGGLETOOTH_BUILD_TESTS)
  add_library(snaggletooth_spc STATIC
    tools/spc/spc_loader.cpp
    tools/spc/wav_writer.cpp
  )
  target_link_libraries(snaggletooth_spc PUBLIC snaggletooth::snaggletooth)
  target_include_directories(snaggletooth_spc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools>
  )
  target_compile_features(snaggletooth_spc PUBLIC cxx_std_20)
  set_target_properties(snaggletooth_spc PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(snaggletooth_spc PRIVATE /W4 /WX)
  else()
    target_compile_options(snaggletooth_spc PRIVATE -Wall -Wextra -Werror)
  endif()

  add_executable(spc_render tools/spc/spc_render_main.cpp)
  target_link_libraries(spc_render PRIVATE snaggletooth_spc)
  if(MSVC)
    target_compile_options(spc_render PRIVATE /W4 /WX)
  else()
    target_compile_options(spc_render PRIVATE -Wall -Wextra -Werror)
  endif()
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

  # Path to the SingleStepTests 65816 "v1" directory (per-opcode-per-mode JSON
  # vectors, native and emulated). When empty, the 65816 vector tests register but
  # skip with a visible reason. Large machine-generated reference data — never
  # vendored.
  set(SNAGGLETOOTH_65816_VECTORS "" CACHE PATH
      "Path to the SingleStepTests 65816 'v1' vector directory")

  add_executable(snaggletooth_tests
    tests/version_test.cpp
    tests/spc700/vector_harness.cpp
    tests/spc700/harness_test.cpp
    tests/spc700/algorithm_test.cpp
    tests/spc700/cycle_engine_test.cpp
    tests/spc700/alu_cycles_test.cpp
    tests/spc700/word_bit_cycles_test.cpp
    tests/spc700/control_flow_cycles_test.cpp
    tests/spc700/vector_test.cpp
    tests/spc700/dummy_read_test.cpp
    tests/cpu65816/vector_harness.cpp
    tests/cpu65816/harness_test.cpp
    tests/cpu65816/algorithm_test.cpp
    tests/cpu65816/cycle_engine_test.cpp
    tests/cpu65816/direct_page_cycles_test.cpp
    tests/cpu65816/absolute_cycles_test.cpp
    tests/cpu65816/indirect_cycles_test.cpp
    tests/cpu65816/stack_cycles_test.cpp
    tests/cpu65816/control_flow_cycles_test.cpp
    tests/cpu65816/interrupt_cycles_test.cpp
    tests/cpu65816/block_move_cycles_test.cpp
    tests/cpu65816/vector_test.cpp
    tests/apu/machine_test.cpp
    tests/apu/port_test.cpp
    tests/apu/timer_test.cpp
    tests/dsp/brr_test.cpp
    tests/dsp/dsp_state_test.cpp
    tests/dsp/pitch_gauss_test.cpp
    tests/dsp/envelope_test.cpp
    tests/dsp/voice_mix_test.cpp
    tests/dsp/noise_master_test.cpp
    tests/dsp/echo_test.cpp
    tests/spc/loader_test.cpp
    tests/spc/wav_test.cpp
    tests/spc/render_test.cpp
  )
  # snaggletooth_spc links snaggletooth::snaggletooth publicly, so the test target
  # reaches the library through it — no separate direct link.
  target_link_libraries(snaggletooth_tests PRIVATE
    snaggletooth_spc
    GTest::gtest_main
  )
  target_compile_definitions(snaggletooth_tests PRIVATE
    SNAGGLETOOTH_SPC700_VECTORS="${SNAGGLETOOTH_SPC700_VECTORS}"
    SNAGGLETOOTH_65816_VECTORS="${SNAGGLETOOTH_65816_VECTORS}")
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
