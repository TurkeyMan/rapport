# Headless checks of the core: run by CI on every platform.
add_executable(rapport-gpu-span-test gpu_span_test.cpp)
target_include_directories(rapport-gpu-span-test PRIVATE ../core/src)
target_link_libraries(rapport-gpu-span-test PRIVATE rapport::rapport)
add_test(NAME gpu-spans COMMAND rapport-gpu-span-test)

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

# Explorer's view, carried from Windows 11's entry's first GetState to its
# second: no COM in it, so it runs everywhere.
if(TARGET rapport_shell)
    add_executable(rapport-explorer-view-test explorer_view_test.cpp)
    target_include_directories(rapport-explorer-view-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shell/src/win32)
    find_package(Threads REQUIRED)
    target_link_libraries(rapport-explorer-view-test PRIVATE rapport::shell Threads::Threads)
endif()

# Lists driven by synthesized input through the real backends, loaded as
# plugins, off the desktop: GTK 3 on Broadway, Qt offscreen. (GTK 4 can't
# synthesize input; win32's lists are native.) GTK 4's entry buttons are
# driven through their focus and activate signals instead.
if(NOT RAPPORT_STATIC AND NOT WIN32 AND NOT APPLE)
    if(TARGET rapport_gtk3)
        find_package(PkgConfig QUIET)
        pkg_check_modules(RAPPORT_TEST_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
        add_executable(rapport-list-input-test-gtk3 list_input_test.cpp)
        target_compile_definitions(rapport-list-input-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-list-input-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-list-input-test-gtk3 rapport_gtk3)
    endif()
    # An entry's buttons on GTK 4: named, focusable, Tab order, menus.
    if(TARGET rapport_gtk4)
        find_package(PkgConfig QUIET)
        pkg_check_modules(RAPPORT_TEST_GTK4 QUIET IMPORTED_TARGET gtk4)
        add_executable(rapport-entry-buttons-test-gtk4 entry_buttons_test.cpp)
        target_link_libraries(rapport-entry-buttons-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-entry-buttons-test-gtk4 rapport_gtk4)
    endif()
    if(TARGET rapport_qt6)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        add_executable(rapport-list-input-test-qt6 list_input_test.cpp)
        target_compile_definitions(rapport-list-input-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-list-input-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-list-input-test-qt6 rapport_qt6)
    endif()
endif()

add_test(NAME i18n COMMAND rapport-i18n-test ${CMAKE_CURRENT_SOURCE_DIR}/i18n)
add_test(NAME http COMMAND rapport-http-test)
add_test(NAME launch COMMAND rapport-launch-test)
add_test(NAME theme-palette COMMAND rapport-theme-palette-test)
add_test(NAME list-press COMMAND rapport-list-press-test)
if(TARGET rapport-explorer-view-test)
    add_test(NAME explorer-view COMMAND rapport-explorer-view-test)
endif()

add_executable(rapport-readback-loss-test readback_loss_test.cpp)
target_compile_definitions(rapport-readback-loss-test PRIVATE RAPPORT_TEST_READBACK)
target_include_directories(rapport-readback-loss-test PRIVATE ../core/src)
target_link_libraries(rapport-readback-loss-test PRIVATE rapport::rapport)
add_test(NAME readback-loss COMMAND rapport-readback-loss-test)
set_tests_properties(readback-loss PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

if(TARGET rapport_win32)
    add_executable(rapport-pane-readback-loss-test pane_readback_loss_test.cpp)
    target_compile_definitions(rapport-pane-readback-loss-test PRIVATE RAPPORT_TEST_READBACK)
    target_include_directories(rapport-pane-readback-loss-test PRIVATE ../core/src)
    target_link_libraries(rapport-pane-readback-loss-test PRIVATE rapport::rapport user32 gdi32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-pane-readback-loss-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-pane-readback-loss-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-pane-readback-loss-test rapport_win32)
    add_test(NAME pane-readback-loss COMMAND rapport-pane-readback-loss-test)
    set_tests_properties(pane-readback-loss PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
endif()
