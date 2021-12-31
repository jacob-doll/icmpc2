set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(pingd_client pingd_client_linux.cpp)

target_link_libraries(pingd_client PRIVATE Threads::Threads)