# Headless checks of the core: run by CI on every platform.
add_executable(rapport-i18n-test i18n_test.cpp)
target_link_libraries(rapport-i18n-test PRIVATE rapport::rapport)

# The theme roles' arithmetic, shared by the backends.
add_executable(rapport-theme-palette-test theme_palette_test.cpp)
target_include_directories(rapport-theme-palette-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
target_link_libraries(rapport-theme-palette-test PRIVATE rapport::rapport)
