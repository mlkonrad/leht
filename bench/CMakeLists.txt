# Benchmarks link MuPDF directly. They measure MuPDF's own threading behaviour,
# which sits below the abstraction core/ presents, so going through the public
# API would hide exactly what we are trying to measure.
find_package(MuPDF REQUIRED)

add_executable(bench_thread_scaling thread_scaling.cpp)
target_link_libraries(bench_thread_scaling
    PRIVATE leht::core MuPDF::MuPDF leht::warnings)
target_include_directories(bench_thread_scaling
    PRIVATE ${CMAKE_SOURCE_DIR}/core/src)

# The cost of process isolation (M3): the same open and render work, in-process
# and through leht-worker.
if(TARGET leht_worker)
    add_executable(bench_worker_latency worker_latency.cpp)
    target_link_libraries(bench_worker_latency PRIVATE leht::core leht::ipc leht::warnings)
    target_compile_definitions(bench_worker_latency PRIVATE
        LEHT_WORKER_EXE="$<TARGET_FILE:leht_worker>")
    add_dependencies(bench_worker_latency leht_worker)
endif()
