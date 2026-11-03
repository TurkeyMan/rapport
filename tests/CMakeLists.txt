# Headless checks of the core: run by CI on every platform.
add_executable(rapport-i18n-test i18n_test.cpp)
target_link_libraries(rapport-i18n-test PRIVATE rapport::rapport)

# HTTP: origins, and requests to a server of its own on the loopback.
add_executable(rapport-http-test http_test.cpp)
target_link_libraries(rapport-http-test PRIVATE rapport::rapport)
if(WIN32)
    target_link_libraries(rapport-http-test PRIVATE ws2_32)
endif()

# Starting programs, as the core and the shell extensions share it.
add_executable(rapport-launch-test launch_test.cpp ../common/launch.cpp)
target_include_directories(rapport-launch-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_compile_features(rapport-launch-test PRIVATE cxx_std_23)
if(WIN32)
    target_link_libraries(rapport-launch-test PRIVATE shell32)
endif()

# The theme roles' arithmetic, shared by the backends.
add_executable(rapport-theme-palette-test theme_palette_test.cpp)
target_include_directories(rapport-theme-palette-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
target_link_libraries(rapport-theme-palette-test PRIVATE rapport::rapport)
