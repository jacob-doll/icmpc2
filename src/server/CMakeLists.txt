set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(pingd_server pingd_server.cpp)

target_link_libraries(pingd_server PRIVATE Threads::Threads)