cmake_minimum_required(VERSION 3.8)
project(dxc-wrapper)

# ===============================================
# Global settings

# require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===============================================
# add submodules

add_subdirectory(extern/dxc)

# ===============================================
# configure executable

file(GLOB_RECURSE SOURCES "src/*.cc" "src/*.hh")

source_group(${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})

add_library(${PROJECT_NAME} STATIC ${SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC src/)
target_link_libraries(${PROJECT_NAME} PUBLIC
    clean-core
    dxc-wrap
)
