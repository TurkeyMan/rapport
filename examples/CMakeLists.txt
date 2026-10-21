add_executable(anyui-demo demo.cpp)
target_link_libraries(anyui-demo PRIVATE anyui::anyui)

add_executable(anyui-gallery gallery.cpp)
target_link_libraries(anyui-gallery PRIVATE anyui::anyui)
target_compile_options(anyui-gallery PRIVATE -Wno-missing-field-initializers)
