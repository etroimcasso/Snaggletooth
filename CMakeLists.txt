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
  src/cartridge.cpp
  src/snes.cpp
  src/snes_dma.cpp
  src/snes_ipl_stub.cpp
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

  # rom_render renders a cartridge instead of an .spc image: it boots the whole
  # machine, so the audio carries the command stream the game's own code sends its
  # sound driver. It shares the WAV writer with spc_render.
  add_executable(rom_render tools/rom/rom_render_main.cpp)
  target_link_libraries(rom_render PRIVATE snaggletooth_spc)
  if(MSVC)
    target_compile_options(rom_render PRIVATE /W4 /WX)
  else()
    target_compile_options(rom_render PRIVATE -Wall -Wextra -Werror)
  endif()

  # The disassembly framework — the tracer and the renderer, which do not depend on
  # which chip the bytes are for. Each chip's disassembler is a backend over it.
  add_library(snaggletooth_disasm STATIC
    tools/disasm/disasm.cpp
  )
  target_include_directories(snaggletooth_disasm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools>
  )
  target_compile_features(snaggletooth_disasm PUBLIC cxx_std_20)
  set_target_properties(snaggletooth_disasm PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(snaggletooth_disasm PRIVATE /W4 /WX)
  else()
    target_compile_options(snaggletooth_disasm PRIVATE -Wall -Wextra -Werror)
  endif()

  # The SPC700 tooling — the disassembler and the assembler. Separate from the .spc
  # tooling above because that reads a file format and this reads the instruction
  # set; the test tree already draws the same line between tests/spc and
  # tests/spc700. The disassembler is the SPC700 backend over the framework, and it
  # links the library because it measures each opcode's cost by running the
  # interpreter rather than carrying a table beside it.
  add_library(snaggletooth_spc700 STATIC
    tools/spc700/spc700_disasm.cpp
  )
  target_link_libraries(snaggletooth_spc700 PUBLIC snaggletooth::snaggletooth snaggletooth_disasm)
  target_include_directories(snaggletooth_spc700 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/spc700>
  )
  target_compile_features(snaggletooth_spc700 PUBLIC cxx_std_20)
  set_target_properties(snaggletooth_spc700 PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(snaggletooth_spc700 PRIVATE /W4 /WX)
  else()
    target_compile_options(snaggletooth_spc700 PRIVATE -Wall -Wextra -Werror)
  endif()

  add_executable(spc700_disasm tools/spc700/spc700_disasm_main.cpp)
  target_link_libraries(spc700_disasm PRIVATE snaggletooth_spc700)
  if(MSVC)
    target_compile_options(spc700_disasm PRIVATE /W4 /WX)
  else()
    target_compile_options(spc700_disasm PRIVATE -Wall -Wextra -Werror)
  endif()

  # The 65816 tooling — the main CPU's disassembler, the 65816 backend over the
  # framework. It links the library for the same reason the SPC700 one does: every
  # opcode's cost is measured by running the core, under each setting of the mode
  # flags, rather than carried in a table beside it.
  add_library(snaggletooth_cpu65816 STATIC
    tools/cpu65816/cpu65816_disasm.cpp
  )
  target_link_libraries(snaggletooth_cpu65816 PUBLIC snaggletooth::snaggletooth snaggletooth_disasm)
  target_include_directories(snaggletooth_cpu65816 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/cpu65816>
  )
  target_compile_features(snaggletooth_cpu65816 PUBLIC cxx_std_20)
  set_target_properties(snaggletooth_cpu65816 PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(snaggletooth_cpu65816 PRIVATE /W4 /WX)
  else()
    target_compile_options(snaggletooth_cpu65816 PRIVATE -Wall -Wextra -Werror)
  endif()

  add_executable(cpu65816_disasm tools/cpu65816/cpu65816_disasm_main.cpp)
  target_link_libraries(cpu65816_disasm PRIVATE snaggletooth_cpu65816)
  if(MSVC)
    target_compile_options(cpu65816_disasm PRIVATE /W4 /WX)
  else()
    target_compile_options(cpu65816_disasm PRIVATE -Wall -Wextra -Werror)
  endif()

  # The cartridge side of the toolkit — the entry points a header names, which
  # disassembler owns each part of the bus, and the whole-cartridge disassembly
  # that traces across banks and writes a source tree. It reads the cartridge
  # through the library's own header functions, so a tool and the machine agree
  # on every byte, and it links both chip backends because a cartridge carries
  # both chips' programs: the main CPU's in its banks, the audio CPU's uploaded
  # at boot.
  add_library(snaggletooth_rom STATIC
    tools/rom/cartridge_entries.cpp
    tools/rom/rom_disasm.cpp
  )
  target_link_libraries(snaggletooth_rom PUBLIC
    snaggletooth::snaggletooth snaggletooth_disasm snaggletooth_cpu65816 snaggletooth_spc700)
  target_include_directories(snaggletooth_rom PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools>
  )
  target_compile_features(snaggletooth_rom PUBLIC cxx_std_20)
  set_target_properties(snaggletooth_rom PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(snaggletooth_rom PRIVATE /W4 /WX)
  else()
    target_compile_options(snaggletooth_rom PRIVATE -Wall -Wextra -Werror)
  endif()

  add_executable(snes_disasm tools/rom/snes_disasm_main.cpp)
  target_link_libraries(snes_disasm PRIVATE snaggletooth_rom)
  if(MSVC)
    target_compile_options(snes_disasm PRIVATE /W4 /WX)
  else()
    target_compile_options(snes_disasm PRIVATE -Wall -Wextra -Werror)
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

  # Path to a directory holding the four Blargg SPC test ROMs (spc_smp.sfc,
  # spc_timer.sfc, spc_mem_access_times.sfc, spc_dsp6.sfc), searched recursively.
  # When empty, the Blargg acceptance tests register but skip with a visible reason.
  # The ROMs are third-party test binaries — staged locally, never vendored.
  set(SNAGGLETOOTH_BLARGG_ROMS "" CACHE PATH
      "Path to the directory holding the Blargg SPC test ROMs")

  # Path to a 64-byte audio boot ROM. When set, the acceptance tests boot the
  # machine on that image instead of the built-in upload stub, which is what a
  # console's own boot code needs to be present for: one Blargg sub-test checksums
  # the boot-ROM window and only the console's own bytes satisfy it. When empty,
  # the tests run the stub and assert its outcome instead. The image is nobody's
  # to distribute — supply your own or leave this unset.
  set(SNAGGLETOOTH_BOOT_ROM "" CACHE FILEPATH
      "Path to a 64-byte audio boot ROM to boot the acceptance tests on")

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
    tests/spc700/disasm_test.cpp
    tests/disasm/framework_test.cpp
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
    tests/cpu65816/disasm_test.cpp
    tests/snes/bus_test.cpp
    tests/snes/cartridge_map_test.cpp
    tests/snes/cartridge_header_test.cpp
    tests/rom/cartridge_entries_test.cpp
    tests/rom/rom_disasm_test.cpp
    tests/snes/machine_test.cpp
    tests/snes/timing_test.cpp
    tests/snes/math_test.cpp
    tests/snes/ppu_stub_test.cpp
    tests/snes/dma_test.cpp
    tests/snes/ipl_stub_test.cpp
    tests/snes/boot_rom_test.cpp
    tests/snes/blargg_test.cpp
    tests/apu/machine_test.cpp
    tests/apu/port_test.cpp
    tests/apu/timer_test.cpp
    tests/apu/cycle_timing_test.cpp
    tests/apu/ipl_rom_test.cpp
    tests/dsp/brr_test.cpp
    tests/dsp/dsp_state_test.cpp
    tests/dsp/pitch_gauss_test.cpp
    tests/dsp/envelope_test.cpp
    tests/dsp/voice_mix_test.cpp
    tests/dsp/noise_master_test.cpp
    tests/dsp/echo_test.cpp
    tests/dsp/sample_schedule_test.cpp
    tests/dsp/filter_history_test.cpp
    tests/spc/loader_test.cpp
    tests/spc/wav_test.cpp
    tests/spc/render_test.cpp
  )
  # snaggletooth_spc links snaggletooth::snaggletooth publicly, and snaggletooth_rom
  # links both chip backends publicly, so the test target reaches the library and
  # the backends through them — no separate direct links.
  target_link_libraries(snaggletooth_tests PRIVATE
    snaggletooth_spc
    snaggletooth_rom
    GTest::gtest_main
  )
  target_compile_definitions(snaggletooth_tests PRIVATE
    SNAGGLETOOTH_SPC700_VECTORS="${SNAGGLETOOTH_SPC700_VECTORS}"
    SNAGGLETOOTH_65816_VECTORS="${SNAGGLETOOTH_65816_VECTORS}"
    SNAGGLETOOTH_BLARGG_ROMS="${SNAGGLETOOTH_BLARGG_ROMS}"
    SNAGGLETOOTH_BOOT_ROM="${SNAGGLETOOTH_BOOT_ROM}")
  # The IPL-stub test seeds the stub into an audio machine through the internal
  # seam in src/, so the test target reaches the private headers there.
  target_include_directories(snaggletooth_tests PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
  if(MSVC)
    target_compile_options(snaggletooth_tests PRIVATE /W4 /WX)
    # std::getenv is flagged C4996 ("unsafe") by MSVC; with /WX that stops the
    # build. It reads a well-known variable name here, so accept the standard CRT
    # function rather than the _s variant.
    target_compile_definitions(snaggletooth_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    # A SnesState is a quarter-megabyte value and a machine holds one; a test
    # naturally places several machines and snapshots on the stack at once. MSVC
    # reserves a 1 MB stack by default where the Unix toolchains reserve 8 MB, so
    # give the test binary the same room.
    target_link_options(snaggletooth_tests PRIVATE /STACK:8388608)
  else()
    target_compile_options(snaggletooth_tests PRIVATE -Wall -Wextra -Werror)
  endif()

  include(GoogleTest)
  gtest_discover_tests(snaggletooth_tests)
endif()
