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

include(CTest)
if(BUILD_TESTING)
    add_executable(sdrive_net_regressions
        src/net.cpp
        src/test_net_regressions.cpp
    )
    target_include_directories(sdrive_net_regressions PRIVATE src)
    if(WIN32)
        target_link_libraries(sdrive_net_regressions PRIVATE ws2_32)
    else()
        target_link_libraries(sdrive_net_regressions PRIVATE Threads::Threads)
    endif()
    add_test(NAME sdrive_http_regressions COMMAND sdrive_net_regressions)
    set_tests_properties(sdrive_http_regressions PROPERTIES TIMEOUT 60)
endif()
