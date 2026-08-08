cmake_minimum_required(VERSION 3.16)
project(sdrive_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(sdrive
    src/main.cpp
    src/config.cpp
    src/net.cpp
    src/storage.cpp
    src/platform.cpp
    src/server.cpp
)

target_include_directories(sdrive PRIVATE src)

if(WIN32)
    target_link_libraries(sdrive PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(sdrive PRIVATE Threads::Threads)
endif()
