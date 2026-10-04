cmake_minimum_required(VERSION 4.4)

project(Allocators LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)



add_library(DarkAllocators)


target_sources(DarkAllocators PUBLIC
        FILE_SET CXX_MODULES TYPE CXX_MODULES
        BASE_DIRS include
        FILES
            include/allocators/darkallocators.cppm
            include/allocators/os_allocator.cppm
            include/allocators/utility.cppm
            include/allocators/tlsf/tlsf_allocator.cppm)

add_executable(example example/main.cpp)

target_link_libraries(example PRIVATE DarkAllocators)
