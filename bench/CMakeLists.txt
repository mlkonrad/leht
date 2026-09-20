# Benchmarks link MuPDF directly. They measure MuPDF's own threading behaviour,
# which sits below the abstraction core/ presents, so going through the public
# API would hide exactly what we are trying to measure.
find_package(MuPDF REQUIRED)

add_executable(bench_thread_scaling thread_scaling.cpp)
target_link_libraries(bench_thread_scaling
    PRIVATE leht::core MuPDF::MuPDF leht::warnings)
target_include_directories(bench_thread_scaling
    PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
