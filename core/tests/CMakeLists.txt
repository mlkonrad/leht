# Plain-assert test executables driven by CTest. No framework dependency:
# core/ has few enough seams that a test framework would cost more than it saves.
# LEHT_SANITIZER_ENV is defined in the top-level CMakeLists.txt, next to the
# option that enables it, so ipc/ and worker/ tests share it too.

function(leht_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE leht::core leht::warnings)
    target_compile_definitions(${name} PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    add_test(NAME ${name} COMMAND ${name})

    # Under sanitizers, suppress the known upstream MuPDF leaks so a real leak
    # in our own code is not lost in the noise. The suppression file documents
    # each entry and points at its reproducer; nothing of ours is suppressed.
    #
    # fast_unwind_on_malloc=0 is required, not optional: LSan's default fast
    # unwinder produces stacks too shallow to reach the frame the suppressions
    # name, so every suppression silently fails to match. It costs speed.
    if(LEHT_SANITIZE)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endfunction()

leht_add_test(test_context)
leht_add_test(test_error)
leht_add_test(test_outline)
leht_add_test(test_renderer)
leht_add_test(test_text)
leht_add_test(test_page_cache)
leht_add_test(test_merge)
leht_add_test(test_compress)
leht_add_test(test_pages)
leht_add_test(test_encrypt)
leht_add_test(test_robustness)
leht_add_test(test_bombs)
leht_add_test(test_open_fd)
leht_add_test(test_edit_save)
leht_add_test(test_redact)
leht_add_test(test_crop_watermark)
leht_add_test(test_annotate)
leht_add_test(test_forms)

# Independent cross-check against poppler. Self-consistency proves nothing about
# correctness; agreement with a different rasteriser does. Skips cleanly (exit
# 77) when pdftoppm, PIL or numpy are absent -- all test-time conveniences.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND LEHT_BUILD_CLI)
    foreach(sample text_10p.pdf text_160p.pdf)
        add_test(NAME crosscheck_${sample}
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_SOURCE_DIR}/tests/render_crosscheck.py
                         $<TARGET_FILE:leht_cli>
                         ${CMAKE_SOURCE_DIR}/tests/corpus/${sample})
        set_tests_properties(crosscheck_${sample} PROPERTIES SKIP_RETURN_CODE 77)
        if(LEHT_SANITIZE)
            set_tests_properties(crosscheck_${sample} PROPERTIES
                ENVIRONMENT "${LEHT_SANITIZER_ENV}")
        endif()
    endforeach()

    add_test(NAME text_crosscheck
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tests/text_crosscheck.py
                     $<TARGET_FILE:leht_cli>
                     ${CMAKE_SOURCE_DIR}/tests/corpus/text_10p.pdf)
    set_tests_properties(text_crosscheck PROPERTIES SKIP_RETURN_CODE 77)
    if(LEHT_SANITIZE)
        set_tests_properties(text_crosscheck PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()

# Corpus replay only -- every corpus file must open or be rejected cleanly.
# Under -DLEHT_SANITIZE=ON this is a genuine memory-safety check.
#
# The MUTATION fuzzer is deliberately NOT in the default suite: it aborts within
# ~22 iterations on a MuPDF 1.28.2 heap-corruption bug we cannot fix from here
# (see tests/crashes/README.md). Wiring a known-red test into CI teaches people
# to ignore CI. Run it by hand:
#     ./build/fuzz/leht_fuzz_open tests/corpus 4000
if(LEHT_BUILD_FUZZ)
    add_test(NAME fuzz_corpus_replay
             COMMAND leht_fuzz_open ${CMAKE_SOURCE_DIR}/tests/corpus 0)
    # Every edit operation over every corpus file, as a regression check.
    add_test(NAME fuzz_edit_corpus_replay
             COMMAND leht_fuzz_edit ${CMAKE_SOURCE_DIR}/tests/corpus 0)
    if(LEHT_SANITIZE)
        set_tests_properties(fuzz_corpus_replay fuzz_edit_corpus_replay PROPERTIES
            ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()

# CLI behaviour: argument parsing and exit codes. The library tests cannot see
# these, which is how `leht rotate -d abc` came to succeed as a no-op.
if(LEHT_BUILD_CLI)
    add_test(NAME cli_arguments
             COMMAND ${CMAKE_SOURCE_DIR}/tests/cli_test.sh
                     $<TARGET_FILE:leht_cli> ${CMAKE_SOURCE_DIR}/tests/corpus)
    if(LEHT_SANITIZE)
        set_tests_properties(cli_arguments PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()
