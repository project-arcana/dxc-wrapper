cmake_minimum_required(VERSION 3.8)
project(DirectXShaderCompilerWrapper)

# ===============================================
# Global settings

# require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DXCW_BUILD_STANDALONE "Build a standalone executable" OFF)

# ===============================================
# add submodules

add_subdirectory(extern/dxc)

# ===============================================
# configure executable

file(GLOB_RECURSE SOURCES "src/*.cc")
file(GLOB_RECURSE HEADERS "src/*.hh")

arcana_add_library(DXCW dxc-wrapper SOURCES HEADERS)

target_include_directories(dxc-wrapper PUBLIC src/)

target_link_libraries(dxc-wrapper PUBLIC
    clean-core
    rich-log
    dxc-wrap
)

if (NOT MSVC)
    target_link_libraries(dxc-wrapper PUBLIC
        -lstdc++fs
    )
endif()

# second option for compat
if (DXCW_BUILD_STANDALONE)
    add_subdirectory(standalone)
endif()
