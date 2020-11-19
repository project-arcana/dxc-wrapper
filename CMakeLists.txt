cmake_minimum_required(VERSION 3.8)
project(dxc-wrapper)

# ===============================================
# Global settings

# require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DXCWRAP_BUILD_STANDALONE "Build a standalone executable" OFF)

# ===============================================
# add submodules

add_subdirectory(extern/dxc)

# ===============================================
# configure executable

file(GLOB_RECURSE SOURCES "src/*.cc" "src/*.hh")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SOURCES})

add_library(${PROJECT_NAME} STATIC ${SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC src/)
target_link_libraries(${PROJECT_NAME} PUBLIC
    clean-core
    rich-log
    dxc-wrap
)

if (NOT MSVC)
    target_link_libraries(${PROJECT_NAME} PUBLIC
        -lstdc++fs
    )
endif()

if (DXCWRAP_BUILD_STANDALONE)
    add_subdirectory(standalone)
endif()
