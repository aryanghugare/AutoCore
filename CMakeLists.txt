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

add_library(vaecu_core STATIC src/can_id.cpp src/can_frame.cpp)
target_include_directories(vaecu_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(vaecu_core PUBLIC cxx_std_17)
vaecu_enable_warnings(vaecu_core)

add_executable(vaecu_sim app/main.cpp)
target_link_libraries(vaecu_sim PRIVATE vaecu_core)
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

    add_executable(vaecu_tests
        tests/build_smoke_test.cpp
        tests/can_frame_test.cpp
        tests/interfaces_test.cpp
    )
    target_link_libraries(vaecu_tests PRIVATE vaecu_core GTest::gtest_main GTest::gmock)
    vaecu_enable_warnings(vaecu_tests)

    include(GoogleTest)
    gtest_discover_tests(vaecu_tests PROPERTIES TIMEOUT 10)
endif()
