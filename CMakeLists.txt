cmake_minimum_required(VERSION 3.25)
project(atom VERSION 0.1.0 LANGUAGES CXX)

# ---------- C++ standard ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- Options ----------
option(ATOM_BUILD_TESTS "Build tests" ON)
option(ATOM_BUILD_BENCH "Build benchmarks" OFF)
set(ATOM_SANITIZER "" CACHE STRING "address,undefined  or  thread")

# ---------- Sanitizers ----------
if(ATOM_SANITIZER)
    add_compile_options(-fsanitize=${ATOM_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${ATOM_SANITIZER})
endif()

find_package(Threads REQUIRED)

# ---------- The library (header-only) ----------
add_library(atom INTERFACE)
add_library(atom::atom ALIAS atom)
target_include_directories(atom INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(atom INTERFACE cxx_std_20)
target_link_libraries(atom INTERFACE Threads::Threads)

# ---------- Warnings (only for our own code) ----------
add_library(atom_warnings INTERFACE)
target_compile_options(atom_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

# ---------- Subfolders ----------
if(ATOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ATOM_BUILD_BENCH)
    add_subdirectory(bench)
endif()