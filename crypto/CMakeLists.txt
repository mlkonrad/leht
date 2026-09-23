# leht::crypto -- signing and verification. OpenSSL only; it never parses a PDF.
# It depends on core/ for leht::Error and ops::ByteRange, the plain types where
# the PDF side of signing and the cryptographic side meet.
#
# libssl is needed for one thing: an https:// timestamp authority.
find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto SSL)
# PKCS#11 tokens (ID cards through OpenSC) are reached through p11-kit, which
# knows the modules the system has registered.
find_package(PkgConfig REQUIRED)
pkg_check_modules(P11KIT REQUIRED IMPORTED_TARGET GLOBAL p11-kit-1)

add_library(lehtcrypto
    src/common.cpp
    src/identity.cpp
    src/pkcs11.cpp
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
    PRIVATE OpenSSL::Crypto OpenSSL::SSL PkgConfig::P11KIT leht::warnings)
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

    # Both halves of signing on real PDFs, checked by pdfsig and qpdf too.
    add_executable(test_pdf_sign tests/test_pdf_sign.cpp)
    target_link_libraries(test_pdf_sign PRIVATE leht::crypto OpenSSL::Crypto leht::warnings)
    target_include_directories(test_pdf_sign PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_pdf_sign PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    add_test(NAME test_pdf_sign COMMAND test_pdf_sign)

    # Keys on a PKCS#11 token, with SoftHSM2 standing in for the ID card.
    # Skipped (77) where SoftHSM is not installed.
    add_executable(test_pkcs11 tests/test_pkcs11.cpp)
    target_link_libraries(test_pkcs11 PRIVATE
        leht::crypto OpenSSL::Crypto PkgConfig::P11KIT leht::warnings)
    target_include_directories(test_pkcs11 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_pkcs11 PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    if(LEHT_BUILD_CLI)
        # The CLI's --pkcs11 and `leht keys`, on the same SoftHSM token.
        target_compile_definitions(test_pkcs11 PRIVATE LEHT_CLI="$<TARGET_FILE:leht_cli>")
    endif()
    add_test(NAME test_pkcs11 COMMAND test_pkcs11)
    set_tests_properties(test_pkcs11 PROPERTIES SKIP_RETURN_CODE 77)

    if(LEHT_SANITIZE)
        set_tests_properties(test_crypto test_pdf_sign test_pkcs11 PROPERTIES
            ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()
