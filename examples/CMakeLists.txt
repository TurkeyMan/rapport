add_executable(rapport-demo demo.cpp)
target_link_libraries(rapport-demo PRIVATE rapport::rapport)

add_executable(rapport-canvas canvas.cpp)
target_link_libraries(rapport-canvas PRIVATE rapport::rapport)

add_executable(rapport-frame frame.cpp)
target_link_libraries(rapport-frame PRIVATE rapport::rapport)
if(WIN32)
    target_link_libraries(rapport-frame PRIVATE d3d11 dxgi)
endif()

add_executable(rapport-gpu gpu.cpp)
target_link_libraries(rapport-gpu PRIVATE rapport::rapport)
foreach(shader scale fill pattern)
    rapport_add_shader(rapport-gpu NAME ${shader} SOURCE shaders/${shader}.hlsl)
endforeach()

add_executable(rapport-print print.cpp)
target_link_libraries(rapport-print PRIVATE rapport::rapport)

add_executable(rapport-gallery gallery.cpp)
target_link_libraries(rapport-gallery PRIVATE rapport::rapport)
if(NOT MSVC)
    target_compile_options(rapport-gallery PRIVATE -Wno-missing-field-initializers)
endif()

# Static builds link the backends in and register them (RAPPORT_BUILTIN_BACKEND)
# from a generated source, since registrations inside a static library are
# dropped by the linker.
if(RAPPORT_STATIC)
    set(builtins_cpp ${CMAKE_CURRENT_BINARY_DIR}/builtin_backends.cpp)
    set(builtins "#include \"rapport/rapport.h\"\n")
    set(backend_targets)
    foreach(backend null gtk3 gtk4 win32)
        if(TARGET rapport::backend_${backend})
            string(APPEND builtins "RAPPORT_BUILTIN_BACKEND(${backend});\n")
            list(APPEND backend_targets rapport::backend_${backend})
        endif()
    endforeach()
    file(GENERATE OUTPUT ${builtins_cpp} CONTENT "${builtins}")
    foreach(example rapport-demo rapport-gallery rapport-canvas rapport-frame rapport-gpu rapport-print)
        target_sources(${example} PRIVATE ${builtins_cpp})
        target_link_libraries(${example} PRIVATE ${backend_targets})
    endforeach()
endif()

if(TARGET rapport::shell)
    rapport_add_shell_extension(rapport-shell-example
        NAME "rapport example" ID org.rapport.Example
        WINDOWS_CLSID {6B1D3F0E-8C2A-4E57-9A41-2F5C7D90B3E8}
        SOURCES shell_example.cpp)
endif()
