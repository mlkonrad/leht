find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)

qt_standard_project_setup()

# The window, view and worker as a library, so both the viewer binary and the
# headless smoke test link the exact same code.
qt_add_library(leht_viewer_lib STATIC
    src/main_window.cpp
    src/page_dialogs.cpp
    src/page_view.cpp
    src/render_worker.cpp
    src/sign_dialog.cpp
    src/thumbnail_bar.cpp
)
target_include_directories(leht_viewer_lib PUBLIC src)
# leht::warnings is deliberately not applied: Qt's moc output and headers do not
# pass our -Werror pedantic set, and they are not ours to fix. core/ stays strict.
# leht::crypto: the viewer is the trusted process. It holds the signing key,
# and leht-worker never does -- see docs/signing.md.
target_link_libraries(leht_viewer_lib PUBLIC leht::core leht::crypto leht::ipc
                      Qt6::Widgets Qt6::PrintSupport)

# The viewer parses nothing itself: every document goes to leht-worker (M3).
# The locations are baked in; render_worker.cpp tries the install tree first
# (relative to the binary, so a relocated or DESTDIR install finds its own
# worker, then the configured prefix) and falls back to the build tree, so
# tests and in-tree runs work unchanged.
file(RELATIVE_PATH LEHT_WORKER_RELATIVE_PATH
     "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBEXECDIR}/leht/leht-worker")
add_dependencies(leht_viewer_lib leht_worker)
target_compile_definitions(leht_viewer_lib PRIVATE
    LEHT_WORKER_RELATIVE_PATH="${LEHT_WORKER_RELATIVE_PATH}"
    LEHT_WORKER_INSTALLED_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/leht/leht-worker"
    LEHT_WORKER_BUILD_PATH="$<TARGET_FILE:leht_worker>")

# The icon is compiled in as well as installed: the window has one when run
# from the build tree, where no icon theme knows about it.
qt_add_executable(leht_viewer src/main.cpp)
qt_add_resources(leht_viewer "icon"
    PREFIX "/leht"
    BASE "${CMAKE_SOURCE_DIR}/data/icons"
    FILES "${CMAKE_SOURCE_DIR}/data/icons/leht.svg")
set_target_properties(leht_viewer PROPERTIES OUTPUT_NAME leht-viewer)
target_compile_definitions(leht_viewer PRIVATE LEHT_APP_ID="${LEHT_APP_ID}")
target_link_libraries(leht_viewer PRIVATE leht_viewer_lib)
install(TARGETS leht_viewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(LEHT_BUILD_TESTS)
    qt_add_executable(ui_smoke_test tests/smoke_test.cpp)
    target_link_libraries(ui_smoke_test PRIVATE leht_viewer_lib)
    target_compile_definitions(ui_smoke_test PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    # The test PKI is generated at run time; no key is committed. p11-kit is
    # for SoftHSM, which stands in for an ID card.
    find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto)
    target_link_libraries(ui_smoke_test PRIVATE OpenSSL::Crypto PkgConfig::P11KIT)
    target_include_directories(ui_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/tests)
    add_test(NAME ui_smoke COMMAND ui_smoke_test)
    # Offscreen so it runs headless in CI; skips (77) if even that is unavailable.
    # detect_leaks=0: Qt leaks a lot at exit under LSan (its own caches, not
    # ours), which would drown the signal. ASan still catches the memory errors
    # that matter here -- use-after-free, overflow -- and one such bug (a reopen
    # freeing the context out from under live documents) was found this way.
    set_tests_properties(ui_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0"
        SKIP_RETURN_CODE 77
        # Normally ~90 s. A modal dialog nobody can dismiss offscreen (e.g.
        # "Could not open document" on a missing corpus) otherwise hangs forever.
        TIMEOUT 300)
endif()
