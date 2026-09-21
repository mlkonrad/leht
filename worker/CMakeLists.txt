# leht-worker: the sandboxed process that parses documents for the viewer (M3).
add_executable(leht_worker
    src/main.cpp
    src/session.cpp
)
set_target_properties(leht_worker PROPERTIES OUTPUT_NAME leht-worker)
target_link_libraries(leht_worker PRIVATE leht::core leht::ipc leht::warnings)

include(GNUInstallDirs)
install(TARGETS leht_worker RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/leht)

if(LEHT_BUILD_TESTS)
    add_executable(test_worker tests/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE leht::core leht::ipc leht::warnings)
    target_include_directories(test_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
    target_compile_definitions(test_worker PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus"
        LEHT_WORKER_EXE="$<TARGET_FILE:leht_worker>")
    add_dependencies(test_worker leht_worker)
    add_test(NAME test_worker COMMAND test_worker)
    if(LEHT_SANITIZE)
        set_tests_properties(test_worker PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()
