# Compiles one HLSL shader for one GPU shader format and writes its bytes as a C++
# header. Run as a script at build time, which `SnaggletoothShaders.cmake` beside
# this file arranges for a consumer's target:
#
#   cmake -DFORMAT=<metallib|spirv|dxil> -DSTAGE=<vert|frag|comp> -DSTEM=<name>
#         -DSRC=<shader.hlsl> -DOUT=<header.h> -DTMP=<scratch directory>
#         [-DINCLUDE=<directory>] [-DUNIFORMS=<n>] [-DREADONLY_BUFFERS=<n>]
#         [-DREADWRITE_TEXTURES=<n>] [-DGLSLANG=<path>] [-DSPIRV_CROSS=<path>]
#         [-DDXC=<path>] -P gen_shader.cmake
#
# The compile path by format:
#   spirv     glslang, HLSL to SPIR-V for Vulkan 1.2.
#   metallib  glslang to SPIR-V, spirv-cross to Metal Shading Language 2.0 with the
#             SPIR-V bindings kept as the Metal argument indices, then the Metal
#             compiler and metallib. SDL's Metal convention puts a shader's uniform
#             buffers, its read-only storage buffers and its read-write storage
#             buffers in one index space, in that order, where SPIR-V keeps them in
#             three sets, so for this format alone the read-only storage buffers'
#             bindings are shifted past the uniforms (UNIFORMS of them) and the
#             read-write storage buffers' past the uniforms and the read-only
#             buffers (READONLY_BUFFERS) and back before the read-write textures
#             (READWRITE_TEXTURES), which share their set and take no buffer index.
#
# INCLUDE is a directory the shader's #include lines are resolved against.
#   dxil      dxc, HLSL to DXIL under shader model 6.0.
#
# The header holds the bytes as an `unsigned char` array and one
# snaggletooth::gpu::ShaderVariants constant (`snaggletooth/gpu/shader_format.h`)
# named after the stem, with the slot for FORMAT filled and the other two empty.
# The entry point's name is `main` for SPIR-V and DXIL and whatever spirv-cross
# named the kernel for Metal, read back from the MSL.

cmake_minimum_required(VERSION 3.20)

foreach(needed FORMAT STAGE STEM SRC OUT TMP)
  if(NOT DEFINED ${needed})
    message(FATAL_ERROR "gen_shader.cmake: ${needed} is not set")
  endif()
endforeach()
foreach(count UNIFORMS READONLY_BUFFERS READWRITE_TEXTURES)
  if(NOT DEFINED ${count})
    set(${count} 0)
  endif()
endforeach()
set(includeFlags)
if(DEFINED INCLUDE)
  set(glslangInclude "-I${INCLUDE}")
  set(dxcInclude -I "${INCLUDE}")
else()
  set(glslangInclude)
  set(dxcInclude)
endif()
math(EXPR readWriteShift "${UNIFORMS} + ${READONLY_BUFFERS} - ${READWRITE_TEXTURES}")
file(MAKE_DIRECTORY "${TMP}")

if(STAGE STREQUAL "vert")
  set(profile "vs_6_0")
elseif(STAGE STREQUAL "frag")
  set(profile "ps_6_0")
elseif(STAGE STREQUAL "comp")
  set(profile "cs_6_0")
else()
  message(FATAL_ERROR "gen_shader.cmake: STAGE is vert, frag or comp, not '${STAGE}'")
endif()

# Runs one compiler and stops the build with its output when it fails.
function(run_tool description)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${description} failed (${code}):\n${out}\n${err}")
  endif()
endfunction()

set(entry "main")
if(FORMAT STREQUAL "dxil")
  if(NOT DEFINED DXC)
    message(FATAL_ERROR "gen_shader.cmake: DXC is not set")
  endif()
  set(bytes "${TMP}/${STEM}.dxil")
  run_tool("dxc on ${SRC}" "${DXC}" -T ${profile} -E main ${dxcInclude} -Fo "${bytes}" "${SRC}")
elseif(FORMAT STREQUAL "spirv")
  if(NOT DEFINED GLSLANG)
    message(FATAL_ERROR "gen_shader.cmake: GLSLANG is not set")
  endif()
  set(bytes "${TMP}/${STEM}.spv")
  run_tool("glslang on ${SRC}" "${GLSLANG}" -D -e main -S ${STAGE} --target-env vulkan1.2 ${glslangInclude}
           -o "${bytes}" "${SRC}")
elseif(FORMAT STREQUAL "metallib")
  if(NOT DEFINED GLSLANG OR NOT DEFINED SPIRV_CROSS)
    message(FATAL_ERROR "gen_shader.cmake: GLSLANG and SPIRV_CROSS must be set")
  endif()
  set(spv "${TMP}/${STEM}.metal.spv")
  set(msl "${TMP}/${STEM}.metal")
  set(air "${TMP}/${STEM}.air")
  set(bytes "${TMP}/${STEM}.metallib")
  run_tool("glslang on ${SRC}" "${GLSLANG}" -D -e main -S ${STAGE} --target-env vulkan1.2 ${glslangInclude}
           --shift-ssbo-binding ${UNIFORMS} 0 --shift-ssbo-binding ${readWriteShift} 1 -o "${spv}" "${SRC}")
  execute_process(COMMAND "${SPIRV_CROSS}" --msl --msl-version 20000 --msl-decoration-binding "${spv}"
                  RESULT_VARIABLE code OUTPUT_VARIABLE text ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "spirv-cross on ${spv} failed (${code}):\n${err}")
  endif()
  file(WRITE "${msl}" "${text}")
  string(REGEX MATCH "(vertex|fragment|kernel) [A-Za-z0-9_]+ ([A-Za-z0-9_]+)\\(" found "${text}")
  if(NOT found)
    message(FATAL_ERROR "spirv-cross left no entry point in ${msl}")
  endif()
  set(entry "${CMAKE_MATCH_2}")
  run_tool("metal on ${msl}" xcrun -sdk macosx metal -c "${msl}" -o "${air}")
  run_tool("metallib on ${air}" xcrun -sdk macosx metallib "${air}" -o "${bytes}")
else()
  message(FATAL_ERROR "gen_shader.cmake: FORMAT is metallib, spirv or dxil, not '${FORMAT}'")
endif()

file(READ "${bytes}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR byteCount "${hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," array "${hex}")
string(REGEX REPLACE "(0x[0-9a-f][0-9a-f],){16}" "\\0\n    " array "${array}")
string(MAKE_C_IDENTIFIER "${STEM}" name)

set(spirvSlot "{}")
set(dxilSlot "{}")
set(metallibSlot "{}")
set(${FORMAT}Slot "{${name}_detail::kBytes, sizeof ${name}_detail::kBytes, \"${entry}\"}")

file(WRITE "${OUT}" "#pragma once

// Generated at build time by gen_shader.cmake from ${STEM} for the ${FORMAT} format
// of this platform, ${byteCount} bytes; not committed.

#include \"snaggletooth/gpu/shader_format.h\"

namespace snaggletooth::shaders {

namespace ${name}_detail {
inline constexpr unsigned char kBytes[] = {
    ${array}
};
}  // namespace ${name}_detail

inline constexpr ::snaggletooth::gpu::ShaderVariants ${name}{
    .spirv = ${spirvSlot},
    .dxil = ${dxilSlot},
    .metallib = ${metallibSlot},
};

}  // namespace snaggletooth::shaders
")
