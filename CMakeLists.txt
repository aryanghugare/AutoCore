cmake_minimum_required(VERSION 3.25)

project(VirtualAutomotiveEcu VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(vaecu_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        )
    endif()
endfunction()

add_executable(vaecu_sim app/main.cpp)
vaecu_enable_warnings(vaecu_sim)

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    if(MSVC)
        set(gtest_force_shared_crt ON CACHE BOOL "Use the project's MSVC runtime" FORCE)
    endif()

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(vaecu_tests tests/build_smoke_test.cpp)
    target_link_libraries(vaecu_tests PRIVATE GTest::gtest_main)
    vaecu_enable_warnings(vaecu_tests)

    include(GoogleTest)
    gtest_discover_tests(vaecu_tests PROPERTIES TIMEOUT 10)
endif()
