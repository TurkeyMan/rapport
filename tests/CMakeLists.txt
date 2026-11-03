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

# A press on a draggable list's row: the rule the backends share.
add_executable(rapport-list-press-test list_press_test.cpp)
target_include_directories(rapport-list-press-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
target_link_libraries(rapport-list-press-test PRIVATE rapport::rapport)

# Lists driven by synthesized input through the real backends, loaded as
# plugins, off the desktop: GTK 3 on Broadway, Qt offscreen. (GTK 4 can't
# synthesize input; win32's lists are native.)
if(NOT RAPPORT_STATIC AND NOT WIN32 AND NOT APPLE)
    if(TARGET rapport_gtk3)
        find_package(PkgConfig QUIET)
        pkg_check_modules(RAPPORT_TEST_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
        add_executable(rapport-list-input-test-gtk3 list_input_test.cpp)
        target_compile_definitions(rapport-list-input-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-list-input-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-list-input-test-gtk3 rapport_gtk3)
    endif()
    if(TARGET rapport_qt6)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        add_executable(rapport-list-input-test-qt6 list_input_test.cpp)
        target_compile_definitions(rapport-list-input-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-list-input-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-list-input-test-qt6 rapport_qt6)
    endif()
endif()
