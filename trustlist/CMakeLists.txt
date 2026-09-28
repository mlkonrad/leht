# leht::trustlist -- the EU trusted lists (M5).
#
# Two libraries on purpose. leht::trustlist is the model and its compact form,
# with no XML: what verifying a signature needs, and all the document worker
# ever links. leht::trustlist_xml reads and verifies the lists themselves
# (libxml2, xmlsec1); only the CLI and `leht-worker --trusted-list` link it.
add_library(lehttrustlist src/model.cpp src/anchor.cpp)
add_library(leht::trustlist ALIAS lehttrustlist)
target_include_directories(lehttrustlist
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lehttrustlist PRIVATE leht::ipc leht::warnings)
set_target_properties(lehttrustlist PROPERTIES POSITION_INDEPENDENT_CODE ON)

# xmlsec1's OpenSSL backend, linked directly: nothing is dlopen()ed, which the
# worker's sandbox would forbid.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XMLSEC REQUIRED IMPORTED_TARGET xmlsec1-openssl libxml-2.0)
find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto)

add_library(lehttrustlist_xml src/xml.cpp)
add_library(leht::trustlist_xml ALIAS lehttrustlist_xml)
target_link_libraries(lehttrustlist_xml
    PUBLIC  leht::trustlist leht::core
    PRIVATE PkgConfig::XMLSEC OpenSSL::Crypto leht::warnings)
set_target_properties(lehttrustlist_xml PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LEHT_BUILD_TESTS)
    # Real lists, as the Commission, Estonia (RIA) and Germany (BNetzA)
    # published them on 24-27 Sep 2026: signatures Leht did not make.
    # Unpacked into the build tree; see tests/README.md.
    set(LEHT_TL_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
    file(MAKE_DIRECTORY ${LEHT_TL_FIXTURES})
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures.tar.xz
         DESTINATION ${LEHT_TL_FIXTURES})

    add_executable(test_trustlist tests/test_trustlist.cpp)
    target_link_libraries(test_trustlist PRIVATE leht::trustlist_xml leht::crypto
                          PkgConfig::XMLSEC OpenSSL::Crypto leht::warnings)
    target_include_directories(test_trustlist PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/tests)
    target_compile_definitions(test_trustlist PRIVATE
        LEHT_TL_FIXTURES="${LEHT_TL_FIXTURES}"
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    add_test(NAME test_trustlist COMMAND test_trustlist)

    if(LEHT_BUILD_CLI)
        # leht trusted-list update|status and verify's verdict, end to end.
        add_executable(test_trustlist_cli tests/test_trustlist_cli.cpp)
        target_link_libraries(test_trustlist_cli PRIVATE leht::trustlist_xml leht::crypto
                              PkgConfig::XMLSEC OpenSSL::Crypto leht::warnings)
        target_include_directories(test_trustlist_cli PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/tests)
        target_compile_definitions(test_trustlist_cli PRIVATE
            LEHT_CLI="$<TARGET_FILE:leht_cli>"
            LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
        add_dependencies(test_trustlist_cli leht_cli)
        add_test(NAME test_trustlist_cli COMMAND test_trustlist_cli)
    endif()
    if(LEHT_SANITIZE)
        set_tests_properties(test_trustlist PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
        if(LEHT_BUILD_CLI)
            set_tests_properties(test_trustlist_cli PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
        endif()
    endif()
endif()
