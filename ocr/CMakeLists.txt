# leht::ocr -- reading words off a picture of a page, with Tesseract. It sees
# pixels only; core/ writes what it reads into the document.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TESSERACT REQUIRED IMPORTED_TARGET GLOBAL tesseract)

add_library(lehtocr src/recognizer.cpp)
add_library(leht::ocr ALIAS lehtocr)
target_include_directories(lehtocr
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Tesseract is PRIVATE for the reason MuPDF is in core/: no public header
# names a Tesseract type.
target_link_libraries(lehtocr
    PUBLIC  leht::core
    PRIVATE PkgConfig::TESSERACT leht::warnings)
set_target_properties(lehtocr PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LEHT_BUILD_TESTS)
    add_executable(test_ocr tests/test_ocr.cpp)
    target_link_libraries(test_ocr PRIVATE leht::ocr leht::warnings)
    target_include_directories(test_ocr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
    target_compile_definitions(test_ocr PRIVATE
        LEHT_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
    add_test(NAME test_ocr COMMAND test_ocr)
    # Skipped (77) when no language data is installed.
    set_tests_properties(test_ocr PROPERTIES SKIP_RETURN_CODE 77)
    if(LEHT_SANITIZE)
        set_tests_properties(test_ocr PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()
