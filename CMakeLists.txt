cmake_minimum_required(VERSION 3.13) # 3.13 required for policy CMP0077
cmake_policy(SET CMP0077 NEW) # make option() ignore normal variables
project(DirectXShaderCompilerWrapper)

# ===============================================
# Global settings

# require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DXCW_BUILD_STANDALONE "Build a standalone executable" OFF)

# Enables Optick profiler integration, expects a CMake target 'OptickCore'
option(DXCW_ENABLE_OPTICK "Enable Optick profiler integration" OFF)

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

if (DXCW_ENABLE_OPTICK)
    if (NOT TARGET OptickCore)
        message(FATAL_ERROR "[DXC Wrapper] DXCW_ENABLE_OPTICK is active, but target OptickCore can't be found (wrong add_subdirectory order?)")
    endif()
    
    message(STATUS "[DXC Wrapper] Optick support enabled")
    target_link_libraries(dxc-wrapper PUBLIC OptickCore)
    target_compile_definitions(dxc-wrapper PRIVATE DXCW_HAS_OPTICK)
endif()
