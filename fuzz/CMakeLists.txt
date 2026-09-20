# The fuzz target builds either way:
#   * with clang, as a libFuzzer binary (coverage-guided, the real thing)
#   * with any compiler, as a standalone corpus-replay + mutation driver
# Neither is worth much without sanitizers: configure with -DLEHT_SANITIZE=ON.
add_executable(leht_fuzz_open fuzz_open.cpp)
target_link_libraries(leht_fuzz_open PRIVATE leht::core leht::warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(leht_fuzz_open PRIVATE LEHT_LIBFUZZER)
    target_compile_options(leht_fuzz_open PRIVATE -fsanitize=fuzzer)
    target_link_options(leht_fuzz_open PRIVATE -fsanitize=fuzzer)
    message(STATUS "  fuzz: libFuzzer (coverage-guided)")
else()
    message(STATUS "  fuzz: standalone driver (install clang for libFuzzer)")
endif()
