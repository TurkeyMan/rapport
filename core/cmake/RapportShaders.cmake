# rapport_add_shader(<target> NAME <name> SOURCE <file.hlsl> [ENTRY <entry>]
#                    [DEFINES <NAME[=value]>...])
#
# Compiles an HLSL pixel shader (Shader Model 5) for the platform being
# built and embeds it in <target>: FXC on Windows (dxbc), DXC or glslang
# elsewhere (spirv), with rap::gpu's bindings (see rapport/gpu.h: textures
# t0..t7 at 0..7, buffers read t8..t11 at 8..11, samplers s0..s7 at 16..23,
# the block b0 at 24, buffers written u0..u3 at 25..28). Files it includes
# from its own folder are depended on (rebuilt when they change) when
# named in DEPENDS. The target can then
#     #include "shaders/<name>.h"
# and call shaders::<name>() for the rap::gpu::shader.
#
# The compiler is found on PATH (and the Windows SDK), or named with
# RAPPORT_SHADER_COMPILER.

# Sets RAPPORT_SHADER_COMPILER (cached) to the compiler found, if any: for
# a project to leave out what needs shaders when there's none.
function(rapport_find_shader_compiler)
    if(RAPPORT_SHADER_COMPILER)
        return()
    endif()
    if(WIN32)
        file(GLOB kits "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64")
        list(SORT kits ORDER DESCENDING)
        find_program(RAPPORT_SHADER_COMPILER fxc HINTS ${kits})
    else()
        find_program(RAPPORT_SHADER_COMPILER NAMES dxc glslangValidator glslang)
    endif()
endfunction()

function(rapport_add_shader target)
    set(_rapport_shaders_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    cmake_parse_arguments(S "" "NAME;SOURCE;ENTRY;STAGE" "DEFINES;DEPENDS" ${ARGN})
    if(NOT S_NAME OR NOT S_SOURCE)
        message(FATAL_ERROR "rapport_add_shader: NAME and SOURCE are needed")
    endif()
    if(NOT S_ENTRY)
        set(S_ENTRY main)
    endif()
    if(S_STAGE STREQUAL "compute")
        set(fxc_profile cs_5_0)
        set(dxc_profile cs_6_0)
        set(glslang_stage comp)
    else()
        set(fxc_profile ps_5_0)
        set(dxc_profile ps_6_0)
        set(glslang_stage frag)
    endif()
    set(depends)
    foreach(d ${S_DEPENDS})
        get_filename_component(d ${d} ABSOLUTE)
        list(APPEND depends ${d})
    endforeach()
    get_filename_component(source_dir ${S_SOURCE} ABSOLUTE)
    get_filename_component(source_dir ${source_dir} DIRECTORY)
    get_filename_component(source ${S_SOURCE} ABSOLUTE)
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${dir})

    rapport_find_shader_compiler()
    if(NOT RAPPORT_SHADER_COMPILER)
        message(FATAL_ERROR "rapport_add_shader: no shader compiler (fxc on Windows; dxc or glslang elsewhere). "
                            "Install one or set RAPPORT_SHADER_COMPILER.")
    endif()
    get_filename_component(compiler_name ${RAPPORT_SHADER_COMPILER} NAME_WE)

    set(defines)
    if(compiler_name STREQUAL "fxc")
        foreach(d ${S_DEFINES})
            list(APPEND defines /D${d})
        endforeach()
        set(language dxbc)
        set(out ${dir}/${S_NAME}.dxbc)
        set(command ${RAPPORT_SHADER_COMPILER} /nologo /T ${fxc_profile} /E ${S_ENTRY} /O3 /I ${source_dir} ${defines} /Fo ${out} ${source})
    elseif(compiler_name STREQUAL "dxc")
        foreach(d ${S_DEFINES})
            list(APPEND defines -D ${d})
        endforeach()
        set(language spirv)
        set(out ${dir}/${S_NAME}.spv)
        set(command ${RAPPORT_SHADER_COMPILER} -spirv -T ${dxc_profile} -E ${S_ENTRY} -O3 -I ${source_dir}
                    -fvk-t-shift 0 0 -fvk-s-shift 16 0 -fvk-b-shift 24 0 -fvk-u-shift 25 0 ${defines} -Fo ${out} ${source})
    else()  # glslang
        foreach(d ${S_DEFINES})
            list(APPEND defines -D${d})
        endforeach()
        set(language spirv)
        set(out ${dir}/${S_NAME}.spv)
        set(command ${RAPPORT_SHADER_COMPILER} -V -D -S ${glslang_stage} -e ${S_ENTRY} --source-entrypoint ${S_ENTRY}
                    -I${source_dir} --shift-texture-binding 0 --shift-sampler-binding 16 --shift-UBO-binding 24
                    --shift-UAV-binding 25 ${defines} -o ${out} ${source})
    endif()

    set(header ${dir}/${S_NAME}.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${command}
        COMMAND ${CMAKE_COMMAND} -DIN=${out} -DOUT=${header} -DNAME=${S_NAME} -DLANGUAGE=${language} -DENTRY=${S_ENTRY}
                -P ${_rapport_shaders_dir}/RapportShaderEmbed.cmake
        DEPENDS ${source} ${depends} ${_rapport_shaders_dir}/RapportShaderEmbed.cmake
        COMMENT "Shader ${S_NAME} (${language})"
        VERBATIM)
    target_sources(${target} PRIVATE ${header})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()
