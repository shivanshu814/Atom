include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(atom_tests
    smoke.cpp
)
target_link_libraries(atom_tests PRIVATE atom::atom atom_warnings GTest::gtest_main)
gtest_discover_tests(atom_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)

