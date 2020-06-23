

# ===============================================
# configure executable

file(GLOB_RECURSE SOURCES "src/*.cc" "src/*.hh")
arcana_source_group(SOURCES)

add_executable(dxc-wrapper-standalone ${SOURCES})
target_include_directories(dxc-wrapper-standalone PUBLIC src/)
target_link_libraries(dxc-wrapper-standalone PUBLIC dxc-wrapper)

if (MSVC)

else()
    # Pthread
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(
        dxc-wrapper-standalone
        PRIVATE
            Threads::Threads
        PUBLIC
            stdc++fs
    )
endif()
