# The shader tools for the platform's GPU shader format, and a compile of one HLSL
# shader into a header a target includes. A consumer that builds Snaggletooth
# through add_subdirectory gets the library's own shader compiled this way; one
# that builds the core another way includes this file and calls:
#
#   snaggletooth_find_shader_tools()
#   snaggletooth_compile_shader(<target> STEM <name> SRC <shader.hlsl> STAGE <vert|frag|comp>
#                               [INCLUDE <directory>] [UNIFORMS <n>] [READONLY_BUFFERS <n>]
#                               [READWRITE_TEXTURES <n>])
#
# The format is the platform's: metallib on Apple systems, dxil on Windows, spirv
# elsewhere. snaggletooth_find_shader_tools finds glslang, spirv-cross, the Metal
# toolchain and dxc as the format needs them, each a cache entry a build may set,
# and stops the configure naming the install when one is missing.
# snaggletooth_compile_shader adds a command producing
# <binary dir>/snaggletooth-shaders/snaggletooth/gpu/shaders/<stem>.h from the
# source through gen_shader.cmake, run again whenever the source or the generator
# changes, and puts <binary dir>/snaggletooth-shaders on the target's include path,
# so the target includes "snaggletooth/gpu/shaders/<stem>.h". INCLUDE is the
# directory the shader's #include lines are resolved against. UNIFORMS,
# READONLY_BUFFERS and READWRITE_TEXTURES are how many of each the shader
# declares, which the Metal compile needs (gen_shader.cmake says why).

set(SNAGGLETOOTH_SHADERS_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(APPLE)
  set(SNAGGLETOOTH_SHADER_FORMAT "metallib")
elseif(WIN32)
  set(SNAGGLETOOTH_SHADER_FORMAT "dxil")
else()
  set(SNAGGLETOOTH_SHADER_FORMAT "spirv")
endif()

function(snaggletooth_find_shader_tools)
  if(SNAGGLETOOTH_SHADER_FORMAT STREQUAL "dxil")
    file(GLOB kits "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
    list(SORT kits)
    list(REVERSE kits)
    find_program(SNAGGLETOOTH_DXC dxc HINTS ${kits})
    if(NOT SNAGGLETOOTH_DXC)
      message(FATAL_ERROR "dxc was not found: the GPU PPU's shader is compiled with it on Windows. "
                          "Install the Windows SDK, or set SNAGGLETOOTH_DXC to dxc.exe, or build "
                          "with -DSNAGGLETOOTH_GPU_PPU=OFF.")
    endif()
    execute_process(COMMAND "${SNAGGLETOOTH_DXC}" --version RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "${SNAGGLETOOTH_DXC} does not run (--version exited ${code}).")
    endif()
    return()
  endif()
  find_program(SNAGGLETOOTH_GLSLANG NAMES glslang glslangValidator HINTS /opt/homebrew/bin /usr/local/bin)
  if(NOT SNAGGLETOOTH_GLSLANG)
    message(FATAL_ERROR "glslang was not found: the GPU PPU's shader is compiled with it. "
                        "Install it (brew install glslang; apt install glslang-tools), or set "
                        "SNAGGLETOOTH_GLSLANG, or build with -DSNAGGLETOOTH_GPU_PPU=OFF.")
  endif()
  if(SNAGGLETOOTH_SHADER_FORMAT STREQUAL "metallib")
    find_program(SNAGGLETOOTH_SPIRV_CROSS spirv-cross HINTS /opt/homebrew/bin /usr/local/bin)
    if(NOT SNAGGLETOOTH_SPIRV_CROSS)
      message(FATAL_ERROR "spirv-cross was not found: the GPU PPU's shader reaches Metal through it. "
                          "Install it (brew install spirv-cross), or set SNAGGLETOOTH_SPIRV_CROSS, or "
                          "build with -DSNAGGLETOOTH_GPU_PPU=OFF.")
    endif()
    execute_process(COMMAND xcrun -sdk macosx metal --version RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "The Metal toolchain does not run (xcrun -sdk macosx metal --version exited "
                          "${code}). Install it with xcodebuild -downloadComponent MetalToolchain, or "
                          "build with -DSNAGGLETOOTH_GPU_PPU=OFF.")
    endif()
  endif()
endfunction()

function(snaggletooth_compile_shader target)
  cmake_parse_arguments(arg "" "STEM;SRC;STAGE;INCLUDE;UNIFORMS;READONLY_BUFFERS;READWRITE_TEXTURES" "" ${ARGN})
  foreach(needed STEM SRC STAGE)
    if(NOT arg_${needed})
      message(FATAL_ERROR "snaggletooth_compile_shader: ${needed} is required")
    endif()
  endforeach()
  foreach(count UNIFORMS READONLY_BUFFERS READWRITE_TEXTURES)
    if(NOT arg_${count})
      set(arg_${count} 0)
    endif()
  endforeach()
  set(includeArg)
  set(includeDeps)
  if(arg_INCLUDE)
    set(includeArg "-DINCLUDE=${arg_INCLUDE}")
    file(GLOB includeDeps "${arg_INCLUDE}/*.hlsl")
  endif()
  set(root "${CMAKE_CURRENT_BINARY_DIR}/snaggletooth-shaders")
  set(header "${root}/snaggletooth/gpu/shaders/${arg_STEM}.h")
  set(scratch "${CMAKE_CURRENT_BINARY_DIR}/snaggletooth-shaders-scratch/${arg_STEM}")
  set(generator "${SNAGGLETOOTH_SHADERS_DIR}/gen_shader.cmake")
  add_custom_command(
    OUTPUT "${header}"
    COMMAND "${CMAKE_COMMAND}"
            -DFORMAT=${SNAGGLETOOTH_SHADER_FORMAT} -DSTAGE=${arg_STAGE} -DSTEM=${arg_STEM}
            -DSRC=${arg_SRC} -DOUT=${header} -DTMP=${scratch} ${includeArg}
            -DUNIFORMS=${arg_UNIFORMS} -DREADONLY_BUFFERS=${arg_READONLY_BUFFERS}
            -DREADWRITE_TEXTURES=${arg_READWRITE_TEXTURES}
            -DGLSLANG=${SNAGGLETOOTH_GLSLANG} -DSPIRV_CROSS=${SNAGGLETOOTH_SPIRV_CROSS}
            -DDXC=${SNAGGLETOOTH_DXC}
            -P "${generator}"
    DEPENDS "${arg_SRC}" "${generator}" ${includeDeps}
    COMMENT "Compiling ${arg_STEM} for ${SNAGGLETOOTH_SHADER_FORMAT}"
    VERBATIM)
  set(shaderTarget "${target}_shader_${arg_STEM}")
  string(MAKE_C_IDENTIFIER "${shaderTarget}" shaderTarget)
  add_custom_target(${shaderTarget} DEPENDS "${header}")
  add_dependencies(${target} ${shaderTarget})
  target_include_directories(${target} PRIVATE "${root}")
endfunction()
