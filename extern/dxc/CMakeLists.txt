cmake_minimum_required(VERSION 3.13)
project(DXCmake)

set(DXCW_ACTIVE_DXC_COMMIT_HASH "416fab6b5c4ba956a320d9131102304da995edfc")
set(DXCW_ACTIVE_DXC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${DXCW_ACTIVE_DXC_COMMIT_HASH}")

file(GLOB_RECURSE SOURCES "lib/*.h")

add_library(dxc-wrap STATIC ${SOURCES} "dummy.cc")

target_include_directories(dxc-wrap PUBLIC "${DXCW_ACTIVE_DXC_COMMIT_HASH}/include/")

if (MSVC)
    target_link_libraries(dxc-wrap
        PUBLIC
            "${DXCW_ACTIVE_DXC_PATH}/lib/dxcompiler.lib"
    )

    # copy the dll to the runtime output folder post-build
    set(DXC_COPY_DLLS "${DXCW_ACTIVE_DXC_PATH}/bin/dxcompiler.dll" "${DXCW_ACTIVE_DXC_PATH}/bin/dxil.dll")

		# we are on MSVC meaning CMAKE_RUNTIME_OUTPUT_DIRECTORY does not contain the /Debug or /Release ending, add it manually
    foreach(file_i ${DXC_COPY_DLLS})
        add_custom_command(
            TARGET dxc-wrap POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${file_i}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIGURATION>"
        )
    endforeach()

else()
    target_link_libraries(dxc-wrap
        PUBLIC
            "${DXCW_ACTIVE_DXC_PATH}/lib_linux/libdxcompiler.so"
    )
endif()
