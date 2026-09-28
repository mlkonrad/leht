# Fuzz targets build either way:
#   * with clang, as libFuzzer binaries (coverage-guided, the real thing)
#   * with any compiler, against standalone_driver.cpp (corpus replay + mutation)
# Neither is worth much without sanitizers: configure with -DLEHT_SANITIZE=ON.
function(leht_add_fuzz_target name source)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(${name} ${source})
        target_compile_definitions(${name} PRIVATE LEHT_LIBFUZZER)
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    else()
        add_executable(${name} ${source} standalone_driver.cpp)
    endif()
    target_link_libraries(${name} PRIVATE leht::core leht::warnings)
endfunction()

leht_add_fuzz_target(leht_fuzz_open fuzz_open.cpp)
leht_add_fuzz_target(leht_fuzz_ops  fuzz_ops.cpp)
leht_add_fuzz_target(leht_fuzz_ipc  fuzz_ipc.cpp)
leht_add_fuzz_target(leht_fuzz_edit fuzz_edit.cpp)
# Signature verification: hostile DER, parsed in the sandboxed worker (M5).
leht_add_fuzz_target(leht_fuzz_verify fuzz_verify.cpp)
# The EU trusted lists: network XML, parsed in the sandboxed worker (queue M5).
leht_add_fuzz_target(leht_fuzz_trustlist fuzz_trustlist.cpp)
target_link_libraries(leht_fuzz_trustlist PRIVATE leht::trustlist_xml)
# Smart-ID and Mobile-ID replies: network JSON, parsed in the trusted process (M6).
leht_add_fuzz_target(leht_fuzz_sk fuzz_sk.cpp)
target_link_libraries(leht_fuzz_sk PRIVATE leht::crypto)
target_include_directories(leht_fuzz_sk PRIVATE ${CMAKE_SOURCE_DIR}/crypto/src)
target_link_libraries(leht_fuzz_ipc PRIVATE leht::ipc)
target_link_libraries(leht_fuzz_verify PRIVATE leht::crypto)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "  fuzz: libFuzzer (coverage-guided)")
else()
    message(STATUS "  fuzz: standalone driver (install clang for libFuzzer)")
endif()
