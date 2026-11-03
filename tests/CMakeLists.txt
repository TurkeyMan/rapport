# Headless checks of the core: run by CI on every platform.
add_executable(rapport-i18n-test i18n_test.cpp)
target_link_libraries(rapport-i18n-test PRIVATE rapport::rapport)

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
