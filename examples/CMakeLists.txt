add_executable(anyui-demo demo.cpp)
target_link_libraries(anyui-demo PRIVATE anyui::anyui)

add_executable(anyui-canvas canvas.cpp)
target_link_libraries(anyui-canvas PRIVATE anyui::anyui)

add_executable(anyui-gallery gallery.cpp)
target_link_libraries(anyui-gallery PRIVATE anyui::anyui)
target_compile_options(anyui-gallery PRIVATE -Wno-missing-field-initializers)

# Static builds link the backends in and register them (ANYUI_BUILTIN_BACKEND)
# from a generated source, since registrations inside a static library are
# dropped by the linker.
if(ANYUI_STATIC)
    set(builtins_cpp ${CMAKE_CURRENT_BINARY_DIR}/builtin_backends.cpp)
    set(builtins "#include \"anyui/anyui.h\"\n")
    set(backend_targets)
    foreach(backend null gtk3 gtk4)
        if(TARGET anyui::backend_${backend})
            string(APPEND builtins "ANYUI_BUILTIN_BACKEND(${backend});\n")
            list(APPEND backend_targets anyui::backend_${backend})
        endif()
    endforeach()
    file(GENERATE OUTPUT ${builtins_cpp} CONTENT "${builtins}")
    foreach(example anyui-demo anyui-gallery anyui-canvas)
        target_sources(${example} PRIVATE ${builtins_cpp})
        target_link_libraries(${example} PRIVATE ${backend_targets})
    endforeach()
endif()
