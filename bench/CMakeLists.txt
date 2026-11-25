add_executable(atom_bench bench_queues.cpp)
target_link_libraries(atom_bench PRIVATE atom::atom atom_warnings)
