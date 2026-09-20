# Plain-assert test executables driven by CTest. No framework dependency:
# core/ has few enough seams that a test framework would cost more than it saves.
function(leht_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE leht::core leht::warnings)
    target_compile_definitions(${name} PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

leht_add_test(test_context)
leht_add_test(test_error)
leht_add_test(test_renderer)
leht_add_test(test_page_cache)
leht_add_test(test_merge)
leht_add_test(test_compress)
leht_add_test(test_pages)
leht_add_test(test_encrypt)

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
    endforeach()
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
endif()
