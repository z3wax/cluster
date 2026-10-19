cmake_minimum_required(VERSION 3.16)
project(sdrive_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SDRIVE_TLS "HTTPS qo'llab-quvvatlash (OpenSSL topilsa)" ON)
option(SDRIVE_STATIC "Statik bog'langan binar (MinGW / Linux)" OFF)

add_library(sdrive_core STATIC
    src/config.cpp
    src/crypto.cpp
    src/net.cpp
    src/storage.cpp
    src/platform.cpp
)
target_include_directories(sdrive_core PUBLIC src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sdrive_core PUBLIC -Wall -Wextra)
endif()

if(SDRIVE_TLS)
    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        target_compile_definitions(sdrive_core PUBLIC SDRIVE_HAVE_OPENSSL)
        target_link_libraries(sdrive_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        message(STATUS "HTTPS: OpenSSL ${OPENSSL_VERSION}")
    else()
        message(WARNING "OpenSSL topilmadi — mijoz faqat http:// serverlar bilan ishlaydi")
    endif()
endif()

if(WIN32)
    target_compile_definitions(sdrive_core PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(sdrive_core PUBLIC ws2_32 winhttp bcrypt shell32 advapi32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(sdrive_core PUBLIC Threads::Threads)
endif()

if(SDRIVE_STATIC)
    target_link_options(sdrive_core PUBLIC -static -static-libgcc -static-libstdc++)
endif()

add_executable(sdrive src/main.cpp src/agent.cpp src/server.cpp)
target_link_libraries(sdrive PRIVATE sdrive_core)

enable_testing()
add_executable(sdrive_tests tests/test_main.cpp)
target_link_libraries(sdrive_tests PRIVATE sdrive_core)
add_test(NAME unit COMMAND sdrive_tests)
