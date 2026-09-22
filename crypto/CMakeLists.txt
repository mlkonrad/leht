# leht::crypto -- signing and verification. OpenSSL only; it never parses a PDF.
# It depends on core/ for leht::Error and ops::ByteRange, the plain types where
# the PDF side of signing and the cryptographic side meet.
#
# libssl is needed for one thing: an https:// timestamp authority.
find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto SSL)

add_library(lehtcrypto
    src/common.cpp
    src/identity.cpp
    src/sign.cpp
    src/tsa.cpp
    src/verify.cpp
)
add_library(leht::crypto ALIAS lehtcrypto)

target_include_directories(lehtcrypto
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# OpenSSL is PRIVATE for the same reason MuPDF is in core/: no public header
# names an OpenSSL type.
target_link_libraries(lehtcrypto
    PUBLIC  leht::core
    PRIVATE OpenSSL::Crypto OpenSSL::SSL leht::warnings)
set_target_properties(lehtcrypto PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LEHT_BUILD_TESTS)
    # The tests build their own PKI (and a TSA) through OpenSSL directly, so no
    # key is ever committed.
    add_executable(test_crypto tests/test_crypto.cpp)
    target_link_libraries(test_crypto PRIVATE leht::crypto OpenSSL::Crypto leht::warnings)
    target_include_directories(test_crypto PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_crypto PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    add_test(NAME test_crypto COMMAND test_crypto)
    if(LEHT_SANITIZE)
        set_tests_properties(test_crypto PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()
