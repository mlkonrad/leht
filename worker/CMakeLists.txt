# leht-worker: the sandboxed process that parses documents for the viewer (M3).
add_executable(leht_worker
    src/main.cpp
    src/session.cpp
    src/sandbox.cpp
)
set_target_properties(leht_worker PROPERTIES OUTPUT_NAME leht-worker)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECCOMP REQUIRED IMPORTED_TARGET libseccomp)
# leht::crypto is here for VERIFICATION only. A signature's CMS blob comes out
# of the document, so it is hostile input and belongs inside the sandbox. The
# signing key never comes near this process.
target_link_libraries(leht_worker PRIVATE leht::core leht::crypto leht::ipc leht::warnings
                      PkgConfig::SECCOMP)
# --ocr=LANGS: the same binary, reading page images for the viewer's OCR.
if(LEHT_WITH_OCR)
    target_sources(leht_worker PRIVATE src/ocr_worker.cpp)
    target_link_libraries(leht_worker PRIVATE leht::ocr)
    target_compile_definitions(leht_worker PRIVATE LEHT_HAVE_OCR)
endif()

include(GNUInstallDirs)
install(TARGETS leht_worker RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/leht)

if(LEHT_BUILD_TESTS)
    add_executable(test_worker tests/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE leht::core leht::crypto leht::ipc leht::warnings)
    target_include_directories(test_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests
                               ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/tests)
    find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto)
    target_link_libraries(test_worker PRIVATE OpenSSL::Crypto)
    target_compile_definitions(test_worker PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus"
        LEHT_WORKER_EXE="$<TARGET_FILE:leht_worker>")
    add_dependencies(test_worker leht_worker)
    if(LEHT_WITH_OCR)
        target_link_libraries(test_worker PRIVATE leht::ocr)
        target_compile_definitions(test_worker PRIVATE LEHT_HAVE_OCR)
    endif()
    add_test(NAME test_worker COMMAND test_worker)
    if(LEHT_SANITIZE)
        set_tests_properties(test_worker PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()

    # Each selftest installs the sandbox, then tries one thing it must forbid.
    # Pass means death by SIGSYS (exit status 128+31 from the shell). A normal
    # exit, even a failing one, means the action was allowed. Sanitizer builds
    # run the worker unsandboxed and report 77, which CTest shows as skipped.
    foreach(what open socket exec fork mmap-exec)
        add_test(NAME worker_sandbox_denies_${what}
            COMMAND sh -c "\"$0\" --selftest-sandbox=${what}; rc=$?; [ $rc -eq 77 ] && exit 77; [ $rc -eq 159 ]"
                    $<TARGET_FILE:leht_worker>)
        set_tests_properties(worker_sandbox_denies_${what} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endif()
