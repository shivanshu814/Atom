include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(atom_tests
    smoke.cpp
    test_mutex_queue.cpp
    test_spsc_queue.cpp
    test_mpsc_queue.cpp
    test_bounded_mpmc_queue.cpp
)
target_link_libraries(atom_tests PRIVATE atom::atom atom_warnings GTest::gtest_main)
gtest_discover_tests(atom_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)

# Passes only on a TSan race report. abort_on_error=0 so ctest can read the output.
if(ATOM_SANITIZER STREQUAL "thread")
    add_executable(atom_negative_spsc negative_spsc.cpp)
    target_compile_definitions(atom_negative_spsc PRIVATE ATOM_SPSC_WEAK_PUBLISH)
    target_link_libraries(atom_negative_spsc PRIVATE atom::atom atom_warnings GTest::gtest_main)
    add_test(NAME NegativeSpsc.TsanCatchesRelaxedPublish COMMAND atom_negative_spsc)
    set_tests_properties(NegativeSpsc.TsanCatchesRelaxedPublish PROPERTIES
        PASS_REGULAR_EXPRESSION "WARNING: ThreadSanitizer: data race"
        ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:abort_on_error=0")
endif()
