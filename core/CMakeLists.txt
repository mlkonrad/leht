find_package(MuPDF REQUIRED)
find_package(Threads REQUIRED)

# No qpdf. MuPDF 1.28 covers everything the plan had assigned to it --
# pdf_graft_page for collision-safe merging, and pdf_write_options for garbage
# collection, stream compression, linearisation and AES-256 encryption with
# full permission flags. Linking a second PDF library for that would be dead
# weight. The qpdf COMMAND-LINE tool is still worth having at test time, as an
# independent checker precisely because it is not the library under test.

add_library(lehtcore
    src/error.cpp
    src/context.cpp
    src/document.cpp
    src/renderer.cpp
    src/page_cache.cpp
    src/ops/merge.cpp
    src/ops/compress.cpp
    src/ops/pages.cpp
    src/ops/encrypt.cpp
)
add_library(leht::core ALIAS lehtcore)

target_include_directories(lehtcore
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# MuPDF is PRIVATE on purpose: leht public headers expose only
# forward declarations, never a MuPDF type. That keeps core/ swappable and
# means consumers do not inherit MuPDF's include path.
target_link_libraries(lehtcore
    PRIVATE MuPDF::MuPDF leht::warnings
    PUBLIC  Threads::Threads)

set_target_properties(lehtcore PROPERTIES
    OUTPUT_NAME lehtcore
    POSITION_INDEPENDENT_CODE ON)
