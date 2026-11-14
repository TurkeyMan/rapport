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

# A window's document tabs: their keys and bookkeeping, then through the
# headless backend.
add_executable(rapport-document-tabs-test document_tabs_test.cpp)
target_include_directories(rapport-document-tabs-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
target_link_libraries(rapport-document-tabs-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-document-tabs-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-document-tabs-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
endif()
add_dependencies(rapport-document-tabs-test rapport_null)
add_test(NAME document-tabs COMMAND rapport-document-tabs-test)
# A stack's pages (layout::stack) through the headless backend.
add_executable(rapport-stack-test stack_test.cpp)
target_link_libraries(rapport-stack-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-stack-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-stack-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
endif()
add_dependencies(rapport-stack-test rapport_null)
add_test(NAME stack COMMAND rapport-stack-test)
# Which keys a window's on_key has from a focused text field.
add_executable(rapport-window-chords-test window_chords_test.cpp)
target_include_directories(rapport-window-chords-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
target_link_libraries(rapport-window-chords-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-window-chords-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-window-chords-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
endif()
add_dependencies(rapport-window-chords-test rapport_null)
add_test(NAME window-chords COMMAND rapport-window-chords-test)
# Where a side button's press goes, for gtk4: the pointer's grab's.
add_executable(rapport-side-buttons-test side_buttons_test.cpp)
target_include_directories(rapport-side-buttons-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
target_compile_features(rapport-side-buttons-test PRIVATE cxx_std_23)

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
        # tree_item::separator_before's rows: the cursor keys step over them.
        add_executable(rapport-tree-separator-keys-test-gtk3 gtk3_tree_separator_keys_test.cpp)
        target_link_libraries(rapport-tree-separator-keys-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-tree-separator-keys-test-gtk3 rapport_gtk3)
        add_executable(rapport-document-tabs-test-gtk3 document_tabs_input_test.cpp)
        target_compile_definitions(rapport-document-tabs-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-document-tabs-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-document-tabs-test-gtk3 rapport_gtk3)
        # Tabs torn off and dropped: GtkNotebook's drag and drop's two ends.
        add_executable(rapport-document-tabs-tearoff-test-gtk3 document_tabs_tearoff_test.cpp)
        target_compile_definitions(rapport-document-tabs-tearoff-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-document-tabs-tearoff-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-document-tabs-tearoff-test-gtk3 rapport_gtk3)
        # A stack's pages (layout::stack) as a GtkStack.
        add_executable(rapport-stack-test-gtk3 stack_toolkit_test.cpp)
        target_compile_definitions(rapport-stack-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-stack-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-stack-test-gtk3 rapport_gtk3)
        # The side buttons and Back/Forward keys as the window's keys.
        add_executable(rapport-back-forward-test-gtk3 back_forward_test.cpp)
        target_compile_definitions(rapport-back-forward-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-back-forward-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-back-forward-test-gtk3 rapport_gtk3)
        # Shortcut chords from a focused entry: the window's first.
        add_executable(rapport-window-chords-test-gtk3 window_chords_input_test.cpp)
        target_compile_definitions(rapport-window-chords-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-window-chords-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-window-chords-test-gtk3 rapport_gtk3)
        # A splitter's on_moved: its dividers' drags only (#30).
        add_executable(rapport-splitter-moved-test-gtk3 splitter_moved_test.cpp)
        target_include_directories(rapport-splitter-moved-test-gtk3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/gtk-common)
        target_link_libraries(rapport-splitter-moved-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-splitter-moved-test-gtk3 rapport_gtk3)
        # A path's own icon from GIO (icon::for_path), and a tree's rows.
        add_executable(rapport-path-icons-test-gtk3 gtk_path_icons_test.cpp)
        target_compile_definitions(rapport-path-icons-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_include_directories(rapport-path-icons-test-gtk3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/gtk-common
                                                                       ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
        target_link_libraries(rapport-path-icons-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-path-icons-test-gtk3 rapport_gtk3)
    endif()
    # An entry's buttons on GTK 4: named, focusable, Tab order, menus.
    if(TARGET rapport_gtk4)
        find_package(PkgConfig QUIET)
        pkg_check_modules(RAPPORT_TEST_GTK4 QUIET IMPORTED_TARGET gtk4)
        add_executable(rapport-entry-buttons-test-gtk4 entry_buttons_test.cpp)
        target_link_libraries(rapport-entry-buttons-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-entry-buttons-test-gtk4 rapport_gtk4)
        add_executable(rapport-document-tabs-test-gtk4 document_tabs_input_test.cpp)
        target_compile_definitions(rapport-document-tabs-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-document-tabs-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-document-tabs-test-gtk4 rapport_gtk4)
        add_executable(rapport-document-tabs-tearoff-test-gtk4 document_tabs_tearoff_test.cpp)
        target_compile_definitions(rapport-document-tabs-tearoff-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-document-tabs-tearoff-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-document-tabs-tearoff-test-gtk4 rapport_gtk4)
        add_executable(rapport-stack-test-gtk4 stack_toolkit_test.cpp)
        target_compile_definitions(rapport-stack-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-stack-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-stack-test-gtk4 rapport_gtk4)
        add_executable(rapport-tree-reinsert-test-gtk4 gtk4_tree_reinsert_test.cpp)
        target_link_libraries(rapport-tree-reinsert-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-tree-reinsert-test-gtk4 rapport_gtk4)
        add_executable(rapport-window-chords-test-gtk4 window_chords_input_test.cpp)
        target_compile_definitions(rapport-window-chords-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-window-chords-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-window-chords-test-gtk4 rapport_gtk4)
        # The hit test behind on_moved (#30), against what GTK 4 picks.
        add_executable(rapport-splitter-moved-test-gtk4 splitter_moved_test.cpp)
        target_include_directories(rapport-splitter-moved-test-gtk4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/gtk-common)
        target_link_libraries(rapport-splitter-moved-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-splitter-moved-test-gtk4 rapport_gtk4)
        add_executable(rapport-path-icons-test-gtk4 gtk_path_icons_test.cpp)
        target_compile_definitions(rapport-path-icons-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_include_directories(rapport-path-icons-test-gtk4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/gtk-common
                                                                       ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
        target_link_libraries(rapport-path-icons-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-path-icons-test-gtk4 rapport_gtk4)
    endif()
    if(TARGET rapport_qt6)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        add_executable(rapport-list-input-test-qt6 list_input_test.cpp)
        target_compile_definitions(rapport-list-input-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-list-input-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-list-input-test-qt6 rapport_qt6)
        add_executable(rapport-document-tabs-test-qt6 document_tabs_input_test.cpp)
        target_compile_definitions(rapport-document-tabs-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-document-tabs-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-document-tabs-test-qt6 rapport_qt6)
        add_executable(rapport-document-tabs-tearoff-test-qt6 document_tabs_tearoff_test.cpp)
        target_compile_definitions(rapport-document-tabs-tearoff-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-document-tabs-tearoff-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-document-tabs-tearoff-test-qt6 rapport_qt6)
        add_executable(rapport-stack-test-qt6 stack_toolkit_test.cpp)
        target_compile_definitions(rapport-stack-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-stack-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-stack-test-qt6 rapport_qt6)
        add_executable(rapport-back-forward-test-qt6 back_forward_test.cpp)
        target_compile_definitions(rapport-back-forward-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-back-forward-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-back-forward-test-qt6 rapport_qt6)
        # QTest's keys go through Qt's shortcuts as a user's do.
        find_package(Qt6 QUIET COMPONENTS Test)
        if(TARGET Qt6::Test)
            add_executable(rapport-window-chords-test-qt6 window_chords_input_test.cpp)
            target_compile_definitions(rapport-window-chords-test-qt6 PRIVATE RAPPORT_TEST_QT6)
            target_link_libraries(rapport-window-chords-test-qt6 PRIVATE rapport::rapport Qt6::Widgets Qt6::Test)
            add_dependencies(rapport-window-chords-test-qt6 rapport_qt6)
        endif()
    endif()
endif()

add_test(NAME i18n COMMAND rapport-i18n-test ${CMAKE_CURRENT_SOURCE_DIR}/i18n)
add_test(NAME http COMMAND rapport-http-test)
add_test(NAME launch COMMAND rapport-launch-test)
add_test(NAME theme-palette COMMAND rapport-theme-palette-test)
add_test(NAME list-press COMMAND rapport-list-press-test)
add_test(NAME side-buttons COMMAND rapport-side-buttons-test)
if(TARGET rapport-explorer-view-test)
    add_test(NAME explorer-view COMMAND rapport-explorer-view-test)
endif()

add_executable(rapport-readback-loss-test readback_loss_test.cpp)
target_compile_definitions(rapport-readback-loss-test PRIVATE RAPPORT_TEST_READBACK)
target_include_directories(rapport-readback-loss-test PRIVATE ../core/src)
target_link_libraries(rapport-readback-loss-test PRIVATE rapport::rapport)
add_test(NAME readback-loss COMMAND rapport-readback-loss-test)
set_tests_properties(readback-loss PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

# Loading a tree's children on demand, on the headless backend.
add_executable(rapport-tree-lazy-test tree_lazy_test.cpp)
target_include_directories(rapport-tree-lazy-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/common)
target_link_libraries(rapport-tree-lazy-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-tree-lazy-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-tree-lazy-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
    if(TARGET rapport_win32)
        target_link_libraries(rapport-tree-lazy-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-tree-lazy-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
endif()
add_dependencies(rapport-tree-lazy-test rapport_null)
add_test(NAME tree-lazy COMMAND rapport-tree-lazy-test)

# tree_item::separator_before through the API (RAPPORT_TREE_TEST_BACKEND).
add_executable(rapport-tree-separator-test tree_separator_test.cpp)
target_link_libraries(rapport-tree-separator-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-tree-separator-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-tree-separator-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
    if(TARGET rapport_win32)
        target_link_libraries(rapport-tree-separator-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-tree-separator-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
endif()
add_dependencies(rapport-tree-separator-test rapport_null)
add_test(NAME tree-separator COMMAND rapport-tree-separator-test)
if(TARGET rapport_win32)
    add_test(NAME tree-separator-win32 COMMAND rapport-tree-separator-test)
    set_tests_properties(tree-separator-win32 PROPERTIES ENVIRONMENT "RAPPORT_TREE_TEST_BACKEND=win32;RAPPORT_NO_ACTIVATE=1")

    add_executable(rapport-win32-tree-separator-test win32_tree_separator_test.cpp)
    target_link_libraries(rapport-win32-tree-separator-test PRIVATE rapport::rapport user32 comctl32 gdi32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-tree-separator-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-tree-separator-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-tree-separator-test rapport_win32)
    add_test(NAME win32-tree-separator COMMAND rapport-win32-tree-separator-test)
    set_tests_properties(win32-tree-separator PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    # Light forced: the line's exact colour checked whatever the desktop's theme.
    add_test(NAME win32-tree-separator-light COMMAND rapport-win32-tree-separator-test)
    set_tests_properties(win32-tree-separator-light PROPERTIES ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light")
endif()

# The side buttons and Back/Forward keys as the window's keys, on Win32 by
# messages sent to its controls.
if(TARGET rapport_win32)
    add_executable(rapport-back-forward-test-win32 back_forward_test.cpp)
    target_compile_definitions(rapport-back-forward-test-win32 PRIVATE RAPPORT_TEST_WIN32)
    target_link_libraries(rapport-back-forward-test-win32 PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-back-forward-test-win32 PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-back-forward-test-win32 PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-back-forward-test-win32 rapport_win32)
    add_test(NAME back-forward-win32 COMMAND rapport-back-forward-test-win32)
    set_tests_properties(back-forward-win32 PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)
endif()

if(TARGET rapport_win32)
    add_executable(rapport-win32-tree-placeholder-test win32_tree_placeholder_test.cpp)
    target_link_libraries(rapport-win32-tree-placeholder-test PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-tree-placeholder-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-tree-placeholder-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-tree-placeholder-test rapport_win32)
    add_test(NAME win32-tree-placeholder COMMAND rapport-win32-tree-placeholder-test)
    set_tests_properties(win32-tree-placeholder PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
endif()

if(TARGET rapport_win32)
    # The lookup on its own, at sizes the test machine's DPI doesn't give.
    add_executable(rapport-win32-shell-icons-test win32_shell_icons_test.cpp ../backends/win32/win32_shell_icons.cpp)
    target_include_directories(rapport-win32-shell-icons-test PRIVATE ../backends/win32)
    target_link_libraries(rapport-win32-shell-icons-test PRIVATE user32 gdi32 shell32 ole32)
    add_test(NAME win32-shell-icons COMMAND rapport-win32-shell-icons-test)

    # A path's own icon, found in the background and told on the asking
    # thread: no window.
    add_executable(rapport-win32-path-icons-test win32_path_icons_test.cpp ../backends/win32/win32_shell_icons.cpp)
    target_include_directories(rapport-win32-path-icons-test PRIVATE ../backends/win32)
    target_link_libraries(rapport-win32-path-icons-test PRIVATE user32 gdi32 shell32 ole32)
    add_test(NAME win32-path-icons COMMAND rapport-win32-path-icons-test)

    # And through a tree, before and after a DPI change.
    add_executable(rapport-win32-shell-icons-dpi-test win32_shell_icons_dpi_test.cpp)
    target_link_libraries(rapport-win32-shell-icons-dpi-test PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-shell-icons-dpi-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-shell-icons-dpi-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-shell-icons-dpi-test rapport_win32)
    add_test(NAME win32-shell-icons-dpi COMMAND rapport-win32-shell-icons-dpi-test)
    set_tests_properties(win32-shell-icons-dpi PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)

    # Painting never asks the shell: reads the backend's cache, so static only.
    if(RAPPORT_STATIC)
        add_executable(rapport-win32-shell-icons-paint-test win32_shell_icons_paint_test.cpp)
        target_include_directories(rapport-win32-shell-icons-paint-test PRIVATE ../backends/win32)
        target_link_libraries(rapport-win32-shell-icons-paint-test PRIVATE rapport::rapport rapport::backend_win32 user32)
        add_test(NAME win32-shell-icons-paint COMMAND rapport-win32-shell-icons-paint-test)
        set_tests_properties(win32-shell-icons-paint PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
        # Paths' own icons on a tree's and a list's rows, never shown, painted
        # into memory: painting never enters the shell; found, they show.
        add_executable(rapport-win32-path-icons-paint-test win32_path_icons_paint_test.cpp)
        target_include_directories(rapport-win32-path-icons-paint-test PRIVATE ../backends/win32)
        target_link_libraries(rapport-win32-path-icons-paint-test PRIVATE rapport::rapport rapport::backend_win32 user32
                                                                          comctl32 shell32)
        add_test(NAME win32-path-icons-paint COMMAND rapport-win32-path-icons-paint-test)
        set_tests_properties(win32-path-icons-paint PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    endif()
endif()

if(TARGET rapport_win32)
    # The document tabs' keys, a tab chosen and a middle click, in a window.
    add_executable(rapport-document-tabs-test-win32 document_tabs_input_test.cpp)
    target_compile_definitions(rapport-document-tabs-test-win32 PRIVATE RAPPORT_TEST_WIN32)
    target_link_libraries(rapport-document-tabs-test-win32 PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-document-tabs-test-win32 PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-document-tabs-test-win32 PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-document-tabs-test-win32 rapport_win32)
    add_test(NAME document-tabs-win32 COMMAND rapport-document-tabs-test-win32)
    add_test(NAME document-tabs-win32-base COMMAND rapport-document-tabs-test-win32 --base)
    set_tests_properties(document-tabs-win32 document-tabs-win32-base PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)
    # Shortcut chords from a focused entry, in a window never shown.
    add_executable(rapport-window-chords-test-win32 window_chords_input_test.cpp)
    target_compile_definitions(rapport-window-chords-test-win32 PRIVATE RAPPORT_TEST_WIN32)
    target_link_libraries(rapport-window-chords-test-win32 PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-window-chords-test-win32 PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-window-chords-test-win32 PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-window-chords-test-win32 rapport_win32)
    add_test(NAME window-chords-win32 COMMAND rapport-window-chords-test-win32)
    add_test(NAME window-chords-win32-dialog COMMAND rapport-window-chords-test-win32 --dialog)
    set_tests_properties(window-chords-win32 window-chords-win32-dialog PROPERTIES TIMEOUT 60)
endif()

if(TARGET rapport_win32)
    add_executable(rapport-win32-frames-test win32_frames_test.cpp)
    target_link_libraries(rapport-win32-frames-test PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-frames-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-frames-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-frames-test rapport_win32)
    add_test(NAME win32-frames COMMAND rapport-win32-frames-test)
    set_tests_properties(win32-frames PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)

    # file_view's first-column inset, printed (it shows a window: CI's): with
    # a Common Controls 6 manifest, and without one, where the controls get
    # whichever comctl32 is selected after the backend initialises (see
    # rapport#41).
    foreach(variant IN ITEMS "" "-no-manifest")
        set(t rapport-win32-file-view-inset${variant}-test)
        add_executable(${t} win32_file_view_inset_test.cpp)
        target_link_libraries(${t} PRIVATE rapport::rapport user32 gdi32 comctl32)
        if(RAPPORT_STATIC)
            target_link_libraries(${t} PRIVATE rapport::backend_win32)
            target_compile_definitions(${t} PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
        endif()
        if(variant)
            target_compile_definitions(${t} PRIVATE RAPPORT_TEST_NO_MANIFEST)
        endif()
        add_dependencies(${t} rapport_win32)
        add_test(NAME win32-file-view-inset${variant} COMMAND ${t})
        set_tests_properties(win32-file-view-inset${variant} PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    endforeach()
endif()

if(TARGET rapport_win32)
    # A stack's pages in a window that's never shown.
    add_executable(rapport-win32-stack-test win32_stack_test.cpp)
    target_link_libraries(rapport-win32-stack-test PRIVATE rapport::rapport user32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-stack-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-stack-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-stack-test rapport_win32)
    add_test(NAME win32-stack COMMAND rapport-win32-stack-test)
    set_tests_properties(win32-stack PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)

    add_executable(rapport-win32-container-test win32_container_test.cpp)
    target_link_libraries(rapport-win32-container-test PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-container-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-container-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-container-test rapport_win32)
    add_test(NAME win32-container COMMAND rapport-win32-container-test)
    set_tests_properties(win32-container PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)

    add_executable(rapport-win32-pane-frames-test win32_pane_frames_test.cpp)
    target_link_libraries(rapport-win32-pane-frames-test PRIVATE rapport::rapport user32 gdi32 dwmapi)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-pane-frames-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-pane-frames-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-pane-frames-test rapport_win32)
    add_test(NAME win32-pane-frames COMMAND rapport-win32-pane-frames-test)
    set_tests_properties(win32-pane-frames PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    add_test(NAME win32-pane-frames-light COMMAND rapport-win32-pane-frames-test)
    set_tests_properties(win32-pane-frames-light PROPERTIES ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light")

    add_executable(rapport-win32-base-surface-test win32_base_surface_test.cpp)
    target_link_libraries(rapport-win32-base-surface-test PRIVATE rapport::rapport user32 gdi32 comctl32 dwmapi)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-base-surface-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-base-surface-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-base-surface-test rapport_win32)
    add_test(NAME win32-base-surface COMMAND rapport-win32-base-surface-test)
    set_tests_properties(win32-base-surface PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    add_test(NAME win32-base-surface-light COMMAND rapport-win32-base-surface-test)
    set_tests_properties(win32-base-surface-light PROPERTIES ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light")

    # The document tabs' row beside its tabs: the window's surface, base or not.
    add_executable(rapport-win32-document-tabs-surface-test win32_document_tabs_surface_test.cpp)
    target_link_libraries(rapport-win32-document-tabs-surface-test PRIVATE rapport::rapport user32 gdi32 comctl32 dwmapi)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-document-tabs-surface-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-document-tabs-surface-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-document-tabs-surface-test rapport_win32)
    add_test(NAME win32-document-tabs-surface COMMAND rapport-win32-document-tabs-surface-test)
    set_tests_properties(win32-document-tabs-surface PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    add_test(NAME win32-document-tabs-surface-light COMMAND rapport-win32-document-tabs-surface-test)
    set_tests_properties(win32-document-tabs-surface-light PROPERTIES ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light")
    # Under high contrast, which only a CI runner may turn on: skipped
    # unless GITHUB_ACTIONS=true and RAPPORT_TEST_SET_HIGH_CONTRAST=1.
    add_executable(rapport-win32-high-contrast-run win32_high_contrast_run.cpp)
    target_link_libraries(rapport-win32-high-contrast-run PRIVATE user32)
    add_test(NAME win32-base-surface-high-contrast
             COMMAND rapport-win32-high-contrast-run $<TARGET_FILE:rapport-win32-base-surface-test>)
    set_tests_properties(win32-base-surface-high-contrast PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

    # The document tabs in the caption on Windows 11 (stage 2), and stage 1
    # kept before it, under a menu bar and in a dialog. Light, dark, on a
    # base surface, and under high contrast (CI only, as above); the DPI and
    # high contrast changed with the window open (--transitions: CI only,
    # skipped unless GITHUB_ACTIONS=true and RAPPORT_TEST_SET_DPI=1 or
    # RAPPORT_TEST_SET_HIGH_CONTRAST=1).
    add_executable(rapport-win32-caption-tabs-test win32_caption_tabs_test.cpp)
    target_link_libraries(rapport-win32-caption-tabs-test PRIVATE rapport::rapport user32 gdi32 comctl32 dwmapi dwrite
                                                                  ole32 oleaut32 uiautomationcore)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-caption-tabs-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-caption-tabs-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-caption-tabs-test rapport_win32)
    add_test(NAME win32-caption-tabs COMMAND rapport-win32-caption-tabs-test)
    add_test(NAME win32-caption-tabs-light COMMAND rapport-win32-caption-tabs-test)
    add_test(NAME win32-caption-tabs-base COMMAND rapport-win32-caption-tabs-test --base)
    add_test(NAME win32-caption-tabs-base-light COMMAND rapport-win32-caption-tabs-test --base)
    set_tests_properties(win32-caption-tabs win32-caption-tabs-base PROPERTIES
        ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 120)
    set_tests_properties(win32-caption-tabs-light win32-caption-tabs-base-light PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" TIMEOUT 120)
    add_test(NAME win32-caption-tabs-high-contrast
             COMMAND rapport-win32-high-contrast-run $<TARGET_FILE:rapport-win32-caption-tabs-test> --base)
    set_tests_properties(win32-caption-tabs-high-contrast PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
    add_test(NAME win32-caption-tabs-transitions COMMAND rapport-win32-caption-tabs-test --transitions)
    set_tests_properties(win32-caption-tabs-transitions PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
    # The toolbar band's glass when a control's own message destroys it
    # (a slider's on_changed closing the window mid-move): never shown.
    add_executable(rapport-win32-caption-glass-reentrant-test win32_caption_glass_reentrant_test.cpp)
    target_link_libraries(rapport-win32-caption-glass-reentrant-test PRIVATE rapport::rapport user32 gdi32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-caption-glass-reentrant-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-caption-glass-reentrant-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-caption-glass-reentrant-test rapport_win32)
    add_test(NAME win32-caption-glass-reentrant COMMAND rapport-win32-caption-glass-reentrant-test)
    set_tests_properties(win32-caption-glass-reentrant PROPERTIES
        ENVIRONMENT RAPPORT_NO_ACTIVATE=1 SKIP_RETURN_CODE 77 TIMEOUT 60)
    # The toolbar band's controls on screen, captured (Windows.Graphics.
    # Capture, C++/WinRT: MSVC only): no holes in the extended frame. Shown;
    # a real skip (77) where there's nothing to capture or tell by, never a
    # pass.
    if(MSVC)
        add_executable(rapport-win32-caption-glass-test win32_caption_glass_test.cpp)
        target_link_libraries(rapport-win32-caption-glass-test PRIVATE rapport::rapport user32 gdi32 dwmapi d3d11 dxgi
                                                                       windowsapp)
        target_compile_options(rapport-win32-caption-glass-test PRIVATE /bigobj)
        if(RAPPORT_STATIC)
            target_link_libraries(rapport-win32-caption-glass-test PRIVATE rapport::backend_win32)
            target_compile_definitions(rapport-win32-caption-glass-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
        endif()
        add_dependencies(rapport-win32-caption-glass-test rapport_win32)
        add_test(NAME win32-caption-glass COMMAND rapport-win32-caption-glass-test)
        add_test(NAME win32-caption-glass-light COMMAND rapport-win32-caption-glass-test)
        set_tests_properties(win32-caption-glass PROPERTIES
            ENVIRONMENT RAPPORT_NO_ACTIVATE=1 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120)
        set_tests_properties(win32-caption-glass-light PROPERTIES
            ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120)
    endif()

    # The toolbar band's metrics and fields (Windows 11, the tabs in the
    # caption, a base surface): never shown but for UIA's part, light and
    # dark, and under high contrast (CI only, as above).
    add_executable(rapport-win32-band-fields-test win32_band_fields_test.cpp)
    target_link_libraries(rapport-win32-band-fields-test PRIVATE rapport::rapport user32 gdi32 comctl32 advapi32
                                                                 ole32 oleaut32 uiautomationcore)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-band-fields-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-band-fields-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-band-fields-test rapport_win32)
    add_test(NAME win32-band-fields COMMAND rapport-win32-band-fields-test)
    set_tests_properties(win32-band-fields PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 120)
    add_test(NAME win32-band-fields-light COMMAND rapport-win32-band-fields-test)
    set_tests_properties(win32-band-fields-light PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" TIMEOUT 120)
    add_test(NAME win32-band-fields-high-contrast
             COMMAND rapport-win32-high-contrast-run $<TARGET_FILE:rapport-win32-band-fields-test>)
    set_tests_properties(win32-band-fields-high-contrast PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
    # The display's scale really changed (125%, 150% where the runner
    # reaches it) and put back: CI only, as win32-caption-tabs-transitions.
    add_test(NAME win32-band-fields-transitions COMMAND rapport-win32-band-fields-test --transitions)
    set_tests_properties(win32-band-fields-transitions PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)

    # The toolbar band's other controls (Windows 11): each the field's
    # height at both densities (compact through its test hook: static
    # builds), and their WinUI faces printed, never shown, light and dark,
    # and under high contrast (CI only, as above).
    add_executable(rapport-win32-band-controls-test win32_band_controls_test.cpp)
    target_link_libraries(rapport-win32-band-controls-test PRIVATE rapport::rapport user32 gdi32 comctl32 advapi32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-band-controls-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-band-controls-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-band-controls-test rapport_win32)
    add_test(NAME win32-band-controls COMMAND rapport-win32-band-controls-test)
    set_tests_properties(win32-band-controls PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME win32-band-controls-light COMMAND rapport-win32-band-controls-test)
    set_tests_properties(win32-band-controls-light PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 TIMEOUT 120)
    # Compact through RAPPORT_DENSITY, for a plugin build (which has no test
    # hook): Windows 11 runners build plugins.
    add_test(NAME win32-band-controls-compact COMMAND rapport-win32-band-controls-test)
    set_tests_properties(win32-band-controls-compact PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_DENSITY=compact" SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME win32-band-controls-compact-light COMMAND rapport-win32-band-controls-test)
    set_tests_properties(win32-band-controls-compact-light PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light;RAPPORT_DENSITY=compact" SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME win32-band-controls-high-contrast
             COMMAND rapport-win32-high-contrast-run $<TARGET_FILE:rapport-win32-band-controls-test>)
    set_tests_properties(win32-band-controls-high-contrast PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
    add_test(NAME win32-band-controls-compact-high-contrast
             COMMAND rapport-win32-high-contrast-run $<TARGET_FILE:rapport-win32-band-controls-test>)
    set_tests_properties(win32-band-controls-compact-high-contrast PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light;RAPPORT_DENSITY=compact" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        TIMEOUT 180)
    # The display's scale really changed (125%, 150% where the runner
    # reaches it) and put back, at each density: CI only, as
    # win32-band-fields-transitions.
    add_test(NAME win32-band-controls-transitions COMMAND rapport-win32-band-controls-test --transitions)
    set_tests_properties(win32-band-controls-transitions PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
    add_test(NAME win32-band-controls-compact-transitions COMMAND rapport-win32-band-controls-test --transitions)
    set_tests_properties(win32-band-controls-compact-transitions PROPERTIES
        ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light;RAPPORT_DENSITY=compact" SKIP_RETURN_CODE 77 RUN_SERIAL
        TRUE TIMEOUT 180)

    # A new text size while windows are open (WM_SETTINGCHANGE, through the
    # text size and message font test hooks: static builds; a plugin build
    # skips): never-shown windows.
    add_executable(rapport-win32-text-size-test win32_text_size_test.cpp)
    target_link_libraries(rapport-win32-text-size-test PRIVATE rapport::rapport user32 gdi32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-text-size-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-text-size-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-text-size-test rapport_win32)
    add_test(NAME win32-text-size COMMAND rapport-win32-text-size-test)
    set_tests_properties(win32-text-size PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 SKIP_RETURN_CODE 77 TIMEOUT 60)

    # The document tabs reordered by dragging and by the keys, in both strips:
    # never-shown windows, given the mouse's messages and the keys.
    add_executable(rapport-win32-document-tabs-reorder-test win32_document_tabs_reorder_test.cpp)
    target_link_libraries(rapport-win32-document-tabs-reorder-test PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-document-tabs-reorder-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-document-tabs-reorder-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-document-tabs-reorder-test rapport_win32)
    add_test(NAME win32-document-tabs-reorder COMMAND rapport-win32-document-tabs-reorder-test)
    set_tests_properties(win32-document-tabs-reorder PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)

    # Tabs dragged out, torn off and dropped into other windows, in both
    # strips: never-shown windows, given the mouse's messages, with test hooks
    # for the z-order and for nothing shown (the drag image, the window let
    # go); --input, the real thing with real input, on CI only.
    add_executable(rapport-win32-document-tabs-tearoff-test win32_document_tabs_tearoff_test.cpp)
    target_link_libraries(rapport-win32-document-tabs-tearoff-test PRIVATE rapport::rapport user32 comctl32 dwmapi)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-document-tabs-tearoff-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-document-tabs-tearoff-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-document-tabs-tearoff-test rapport_win32)
    add_test(NAME win32-document-tabs-tearoff COMMAND rapport-win32-document-tabs-tearoff-test)
    set_tests_properties(win32-document-tabs-tearoff PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)
    # Real input on shown windows: skipped (77) unless GITHUB_ACTIONS=true and
    # RAPPORT_TEST_SEND_INPUT=1.
    add_test(NAME win32-document-tabs-tearoff-input COMMAND rapport-win32-document-tabs-tearoff-test --input)
    set_tests_properties(win32-document-tabs-tearoff-input PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 120)

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
    add_test(NAME pane-frame-order COMMAND rapport-pane-readback-loss-test --ordering)
    set_tests_properties(pane-readback-loss pane-frame-order PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
endif()

add_executable(rapport-watch-lifetime-test watch_lifetime_test.cpp)
target_link_libraries(rapport-watch-lifetime-test PRIVATE rapport::rapport)
foreach(mode ordinary self)
    add_test(NAME watch-${mode}
        COMMAND ${CMAKE_COMMAND} -DWATCH=$<TARGET_FILE:rapport-watch-lifetime-test>
                -DMODE=${mode} -DDIR=${CMAKE_CURRENT_BINARY_DIR}/watch-${mode}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/watch_lifetime.cmake)
endforeach()

if(TARGET rapport_win32)
    add_executable(rapport-win32-trace-test win32_trace_test.cpp ../backends/win32/win32_trace.cpp)
    target_include_directories(rapport-win32-trace-test PRIVATE ../backends/win32)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME win32-trace COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/win32_trace_test.py $<TARGET_FILE:rapport-win32-trace-test>)
    endif()
endif()

if(TARGET rapport_win32)
    add_executable(rapport-win32-trace-splitter-test win32_trace_splitter_test.cpp)
    target_link_libraries(rapport-win32-trace-splitter-test PRIVATE rapport::rapport user32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-trace-splitter-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-trace-splitter-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-trace-splitter-test rapport_win32)
    add_test(NAME win32-trace-splitter COMMAND rapport-win32-trace-splitter-test)
    set_tests_properties(win32-trace-splitter PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)

    # Common controls 6 whatever the app's manifest (#41): the toolchain's
    # default, and (MSVC) none at all.
    foreach(variant IN ITEMS default no-manifest)
        if(variant STREQUAL "no-manifest" AND NOT MSVC)
            continue()
        endif()
        set(target rapport-win32-comctl6-test)
        if(variant STREQUAL "no-manifest")
            set(target rapport-win32-comctl6-test-no-manifest)
        endif()
        add_executable(${target} win32_comctl6_test.cpp)
        target_link_libraries(${target} PRIVATE rapport::rapport user32 shlwapi)
        if(variant STREQUAL "no-manifest")
            target_link_options(${target} PRIVATE /MANIFEST:NO)
        endif()
        if(RAPPORT_STATIC)
            target_link_libraries(${target} PRIVATE rapport::backend_win32)
            target_compile_definitions(${target} PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
        endif()
        add_dependencies(${target} rapport_win32)
        string(REPLACE "rapport-" "" test_name ${target})
        string(REPLACE "-test" "" test_name ${test_name})
        add_test(NAME ${test_name} COMMAND ${target})
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    endforeach()

    add_executable(rapport-win32-splitter-sizes-test win32_splitter_sizes_test.cpp)
    target_link_libraries(rapport-win32-splitter-sizes-test PRIVATE rapport::rapport user32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-splitter-sizes-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-splitter-sizes-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-splitter-sizes-test rapport_win32)
    add_test(NAME win32-splitter-sizes COMMAND rapport-win32-splitter-sizes-test)
    set_tests_properties(win32-splitter-sizes PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)

    add_executable(rapport-win32-thin-divider-test win32_thin_divider_test.cpp)
    target_link_libraries(rapport-win32-thin-divider-test PRIVATE rapport::rapport user32 gdi32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-thin-divider-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-thin-divider-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-thin-divider-test rapport_win32)
    add_test(NAME win32-thin-divider COMMAND rapport-win32-thin-divider-test)
    add_test(NAME win32-thin-divider-light COMMAND rapport-win32-thin-divider-test)
    set_tests_properties(win32-thin-divider PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
    set_tests_properties(win32-thin-divider-light PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "RAPPORT_NO_ACTIVATE=1;RAPPORT_THEME=light")
endif()

if(WIN32)
    add_executable(rapport-shell-overlay-names-test shell_overlay_names_test.cpp)
    target_compile_features(rapport-shell-overlay-names-test PRIVATE cxx_std_23)
    target_include_directories(rapport-shell-overlay-names-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../shell/src/win32)
    add_test(NAME shell-overlay-names COMMAND rapport-shell-overlay-names-test)
endif()

if(WIN32)
    add_executable(rapport-win32-menu-dismiss-test win32_menu_dismiss_test.cpp)
    target_compile_features(rapport-win32-menu-dismiss-test PRIVATE cxx_std_23)
    target_include_directories(rapport-win32-menu-dismiss-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/win32)
    target_link_libraries(rapport-win32-menu-dismiss-test PRIVATE user32)
    add_test(NAME win32-menu-dismiss COMMAND rapport-win32-menu-dismiss-test)
endif()

if(TARGET rapport_win32)
    add_executable(rapport-win32-list-stretch-test win32_list_stretch_test.cpp)
    target_link_libraries(rapport-win32-list-stretch-test PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-list-stretch-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-list-stretch-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-list-stretch-test rapport_win32)
    add_test(NAME win32-list-stretch COMMAND rapport-win32-list-stretch-test)
    set_tests_properties(win32-list-stretch PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1)
endif()

# UI Automation for drawn elements: a caption tab strip's, through an
# in-process UIA client on a thread of its own. The provider is compiled in
# directly, as nothing of rapport's uses it yet; its window is shown without
# activating and never given input.
if(TARGET rapport_win32)
    add_executable(rapport-win32-uia-test win32_uia_test.cpp ../backends/win32/win32_uia.cpp)
    target_include_directories(rapport-win32-uia-test PRIVATE ../backends/win32)
    target_compile_features(rapport-win32-uia-test PRIVATE cxx_std_23)
    target_compile_definitions(rapport-win32-uia-test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
                                                              _WIN32_WINNT=0x0A00)
    target_link_libraries(rapport-win32-uia-test PRIVATE user32 ole32 oleaut32 uiautomationcore)
    add_test(NAME win32-uia COMMAND rapport-win32-uia-test)
    set_tests_properties(win32-uia PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 90)
endif()

# Several windows in one process: ui::save_snapshot's fallback and a given
# window; window::begin_busy, counted and each window's own. On the null
# backend the counts only; on Win32 with windows never shown; GTK and Qt
# off the desktop (offscreen.sh), so built here and run by hand.
add_executable(rapport-busy-snapshot-test busy_snapshot_test.cpp)
target_link_libraries(rapport-busy-snapshot-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-busy-snapshot-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-busy-snapshot-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
endif()
add_dependencies(rapport-busy-snapshot-test rapport_null)
add_test(NAME busy-snapshot COMMAND rapport-busy-snapshot-test)
if(TARGET rapport_win32)
    add_executable(rapport-busy-snapshot-test-win32 busy_snapshot_test.cpp)
    target_compile_definitions(rapport-busy-snapshot-test-win32 PRIVATE RAPPORT_TEST_WIN32)
    target_link_libraries(rapport-busy-snapshot-test-win32 PRIVATE rapport::rapport user32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-busy-snapshot-test-win32 PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-busy-snapshot-test-win32 PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-busy-snapshot-test-win32 rapport_win32)
    add_test(NAME busy-snapshot-win32 COMMAND rapport-busy-snapshot-test-win32)
    set_tests_properties(busy-snapshot-win32 PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)
endif()
if(NOT RAPPORT_STATIC AND NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(TARGET rapport_gtk3)
        pkg_check_modules(RAPPORT_TEST_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
        add_executable(rapport-busy-snapshot-test-gtk3 busy_snapshot_test.cpp)
        target_compile_definitions(rapport-busy-snapshot-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-busy-snapshot-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-busy-snapshot-test-gtk3 rapport_gtk3)
    endif()
    if(TARGET rapport_gtk4)
        pkg_check_modules(RAPPORT_TEST_GTK4 QUIET IMPORTED_TARGET gtk4)
        add_executable(rapport-busy-snapshot-test-gtk4 busy_snapshot_test.cpp)
        target_compile_definitions(rapport-busy-snapshot-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-busy-snapshot-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-busy-snapshot-test-gtk4 rapport_gtk4)
    endif()
    if(TARGET rapport_qt6)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        add_executable(rapport-busy-snapshot-test-qt6 busy_snapshot_test.cpp)
        target_compile_definitions(rapport-busy-snapshot-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-busy-snapshot-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-busy-snapshot-test-qt6 rapport_qt6)
    endif()
endif()

# window::on_destroyed: once, before the window's widgets go, whoever
# destroys it (the app, or its on_finished after a close); not once cleared;
# the last one set. On the null backend; on Win32 with windows never shown;
# GTK on Broadway and Qt offscreen, which CI runs.
add_executable(rapport-on-destroyed-test on_destroyed_test.cpp)
target_link_libraries(rapport-on-destroyed-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-on-destroyed-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-on-destroyed-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
endif()
add_dependencies(rapport-on-destroyed-test rapport_null)
add_test(NAME on-destroyed COMMAND rapport-on-destroyed-test)
if(TARGET rapport_win32)
    add_executable(rapport-on-destroyed-test-win32 on_destroyed_test.cpp)
    target_compile_definitions(rapport-on-destroyed-test-win32 PRIVATE RAPPORT_TEST_WIN32)
    target_link_libraries(rapport-on-destroyed-test-win32 PRIVATE rapport::rapport user32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-on-destroyed-test-win32 PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-on-destroyed-test-win32 PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-on-destroyed-test-win32 rapport_win32)
    add_test(NAME on-destroyed-win32 COMMAND rapport-on-destroyed-test-win32)
    set_tests_properties(on-destroyed-win32 PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)
endif()
if(NOT RAPPORT_STATIC AND NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(TARGET rapport_gtk3)
        pkg_check_modules(RAPPORT_TEST_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
        add_executable(rapport-on-destroyed-test-gtk3 on_destroyed_test.cpp)
        target_compile_definitions(rapport-on-destroyed-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-on-destroyed-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-on-destroyed-test-gtk3 rapport_gtk3)
    endif()
    if(TARGET rapport_gtk4)
        pkg_check_modules(RAPPORT_TEST_GTK4 QUIET IMPORTED_TARGET gtk4)
        add_executable(rapport-on-destroyed-test-gtk4 on_destroyed_test.cpp)
        target_compile_definitions(rapport-on-destroyed-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-on-destroyed-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-on-destroyed-test-gtk4 rapport_gtk4)
    endif()
    if(TARGET rapport_qt6)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        add_executable(rapport-on-destroyed-test-qt6 on_destroyed_test.cpp)
        target_compile_definitions(rapport-on-destroyed-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-on-destroyed-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-on-destroyed-test-qt6 rapport_qt6)
    endif()
endif()

# window::present: shown, restored from minimised (maximised again if it
# was), shown again once closed; with RAPPORT_NO_ACTIVATE the focus left
# alone. Win32 shows and minimises windows, so only on a CI runner
# (RAPPORT_TEST_SEND_INPUT), else skipped; GTK on Broadway, Qt offscreen.
if(TARGET rapport_win32)
    add_executable(rapport-present-test-win32 present_test.cpp)
    target_compile_definitions(rapport-present-test-win32 PRIVATE RAPPORT_TEST_WIN32)
    target_link_libraries(rapport-present-test-win32 PRIVATE rapport::rapport user32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-present-test-win32 PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-present-test-win32 PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-present-test-win32 rapport_win32)
    add_test(NAME win32-present COMMAND rapport-present-test-win32)
    set_tests_properties(win32-present PROPERTIES
        ENVIRONMENT RAPPORT_NO_ACTIVATE=1 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME win32-present-activate COMMAND rapport-present-test-win32)
    set_tests_properties(win32-present-activate PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 60)
endif()
if(NOT RAPPORT_STATIC AND NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(TARGET rapport_gtk3)
        pkg_check_modules(RAPPORT_TEST_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
        add_executable(rapport-present-test-gtk3 present_test.cpp)
        target_compile_definitions(rapport-present-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-present-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-present-test-gtk3 rapport_gtk3)
    endif()
    if(TARGET rapport_gtk4)
        pkg_check_modules(RAPPORT_TEST_GTK4 QUIET IMPORTED_TARGET gtk4)
        add_executable(rapport-present-test-gtk4 present_test.cpp)
        target_compile_definitions(rapport-present-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-present-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-present-test-gtk4 rapport_gtk4)
    endif()
    if(TARGET rapport_qt6)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        add_executable(rapport-present-test-qt6 present_test.cpp)
        target_compile_definitions(rapport-present-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-present-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-present-test-qt6 rapport_qt6)
    endif()
endif()

# The app's setters (entry, drop-down, combo) don't fire on_changed; the
# user's typing does. Null, Win32 never shown, GTK on Broadway, Qt offscreen.
add_executable(rapport-setters-quiet-test setters_quiet_test.cpp)
target_link_libraries(rapport-setters-quiet-test PRIVATE rapport::rapport)
if(RAPPORT_STATIC)
    target_link_libraries(rapport-setters-quiet-test PRIVATE rapport::backend_null)
    target_compile_definitions(rapport-setters-quiet-test PRIVATE RAPPORT_TEST_BUILTIN_NULL)
endif()
add_dependencies(rapport-setters-quiet-test rapport_null)
add_test(NAME setters-quiet COMMAND rapport-setters-quiet-test)
if(TARGET rapport_win32)
    add_executable(rapport-setters-quiet-test-win32 setters_quiet_test.cpp)
    target_compile_definitions(rapport-setters-quiet-test-win32 PRIVATE RAPPORT_TEST_WIN32)
    target_link_libraries(rapport-setters-quiet-test-win32 PRIVATE rapport::rapport user32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-setters-quiet-test-win32 PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-setters-quiet-test-win32 PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-setters-quiet-test-win32 rapport_win32)
    add_test(NAME setters-quiet-win32 COMMAND rapport-setters-quiet-test-win32)
    set_tests_properties(setters-quiet-win32 PROPERTIES ENVIRONMENT RAPPORT_NO_ACTIVATE=1 TIMEOUT 60)
endif()
if(NOT RAPPORT_STATIC AND NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(TARGET rapport_gtk3)
        pkg_check_modules(RAPPORT_TEST_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
        add_executable(rapport-setters-quiet-test-gtk3 setters_quiet_test.cpp)
        target_compile_definitions(rapport-setters-quiet-test-gtk3 PRIVATE RAPPORT_TEST_GTK3)
        target_link_libraries(rapport-setters-quiet-test-gtk3 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK3)
        add_dependencies(rapport-setters-quiet-test-gtk3 rapport_gtk3)
    endif()
    if(TARGET rapport_gtk4)
        pkg_check_modules(RAPPORT_TEST_GTK4 QUIET IMPORTED_TARGET gtk4)
        add_executable(rapport-setters-quiet-test-gtk4 setters_quiet_test.cpp)
        target_compile_definitions(rapport-setters-quiet-test-gtk4 PRIVATE RAPPORT_TEST_GTK4)
        target_link_libraries(rapport-setters-quiet-test-gtk4 PRIVATE rapport::rapport PkgConfig::RAPPORT_TEST_GTK4)
        add_dependencies(rapport-setters-quiet-test-gtk4 rapport_gtk4)
    endif()
    if(TARGET rapport_qt6)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        add_executable(rapport-setters-quiet-test-qt6 setters_quiet_test.cpp)
        target_compile_definitions(rapport-setters-quiet-test-qt6 PRIVATE RAPPORT_TEST_QT6)
        target_link_libraries(rapport-setters-quiet-test-qt6 PRIVATE rapport::rapport Qt6::Widgets)
        add_dependencies(rapport-setters-quiet-test-qt6 rapport_qt6)
    endif()
endif()

# A toolbar's buttons from the keyboard: one Tab stop, the arrows, Home,
# End, Space and Enter, focus() and has_focus() per button. Shown and
# focused, so only on a CI runner (RAPPORT_TEST_SEND_INPUT); else skipped.
if(TARGET rapport_win32)
    add_executable(rapport-win32-toolbar-keyboard-test win32_toolbar_keyboard_test.cpp)
    target_link_libraries(rapport-win32-toolbar-keyboard-test PRIVATE rapport::rapport user32 comctl32)
    if(RAPPORT_STATIC)
        target_link_libraries(rapport-win32-toolbar-keyboard-test PRIVATE rapport::backend_win32)
        target_compile_definitions(rapport-win32-toolbar-keyboard-test PRIVATE RAPPORT_TEST_BUILTIN_WIN32)
    endif()
    add_dependencies(rapport-win32-toolbar-keyboard-test rapport_win32)
    add_test(NAME win32-toolbar-keyboard COMMAND rapport-win32-toolbar-keyboard-test)
    set_tests_properties(win32-toolbar-keyboard PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 60)
endif()
