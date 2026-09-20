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

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "  fuzz: libFuzzer (coverage-guided)")
else()
    message(STATUS "  fuzz: standalone driver (install clang for libFuzzer)")
endif()
