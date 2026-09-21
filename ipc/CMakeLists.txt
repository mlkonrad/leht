# The viewer <-> leht-worker wire format. No Qt and no MuPDF: it depends on
# core/ only for the plain value types (Bitmap, PageSize, TextQuad) it
# serializes, which is exactly the surface core/ promises is MuPDF-free.
add_library(lehtipc
    src/wire.cpp
    src/protocol.cpp
    src/channel.cpp
    src/process.cpp
)
add_library(leht::ipc ALIAS lehtipc)

target_include_directories(lehtipc
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(lehtipc
    PUBLIC  leht::core
    PRIVATE leht::warnings)
set_target_properties(lehtipc PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LEHT_BUILD_TESTS)
    add_executable(test_ipc tests/test_ipc.cpp)
    target_link_libraries(test_ipc PRIVATE leht::ipc leht::warnings)
    target_include_directories(test_ipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
    add_test(NAME test_ipc COMMAND test_ipc)
    if(LEHT_SANITIZE)
        set_tests_properties(test_ipc PROPERTIES ENVIRONMENT "${LEHT_SANITIZER_ENV}")
    endif()
endif()
