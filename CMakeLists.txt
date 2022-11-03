cmake_minimum_required(VERSION 3.20)

project(ompt_tt VERSION 1.0 DESCRIPTION "OMPT Test Tool" LANGUAGES CXX C)

find_package(OpenMP REQUIRED)

add_library(ompt_tt SHARED src/callbacks.cpp)
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "NVHPC")
    target_compile_options(ompt_tt PRIVATE "-mp=ompt,gpu")
    target_link_options(ompt_tt PRIVATE "-mp=ompt,gpu")
else()
    target_link_libraries(ompt_tt PUBLIC OpenMP::OpenMP_CXX)
endif()

add_executable(simple src/matmult.c src/matmult_initialize.c src/matmult_initialize.h)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "NVHPC")
    # Use the tool-enabled OpenMP runtime
    target_compile_options(simple PRIVATE "-mp=ompt,gpu")
    target_link_options(simple PRIVATE "-mp=ompt,gpu")
    target_link_libraries(simple PUBLIC ompt_tt)
else()
    target_link_libraries(simple PUBLIC ompt_tt OpenMP::OpenMP_CXX)
endif()
