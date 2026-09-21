find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

# The window, view and worker as a library, so both the viewer binary and the
# headless smoke test link the exact same code.
qt_add_library(leht_viewer_lib STATIC
    src/main_window.cpp
    src/page_view.cpp
    src/render_worker.cpp
)
target_include_directories(leht_viewer_lib PUBLIC src)
# leht::warnings is deliberately not applied: Qt's moc output and headers do not
# pass our -Werror pedantic set, and they are not ours to fix. core/ stays strict.
target_link_libraries(leht_viewer_lib PUBLIC leht::core Qt6::Widgets)

qt_add_executable(leht_viewer src/main.cpp)
set_target_properties(leht_viewer PROPERTIES OUTPUT_NAME leht-viewer)
target_link_libraries(leht_viewer PRIVATE leht_viewer_lib)

if(LEHT_BUILD_TESTS)
    qt_add_executable(ui_smoke_test tests/smoke_test.cpp)
    target_link_libraries(ui_smoke_test PRIVATE leht_viewer_lib)
    target_compile_definitions(ui_smoke_test PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    add_test(NAME ui_smoke COMMAND ui_smoke_test)
    # Offscreen so it runs headless in CI; skips (77) if even that is unavailable.
    set_tests_properties(ui_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        SKIP_RETURN_CODE 77)
endif()
