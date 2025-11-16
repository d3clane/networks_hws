cmake_minimum_required(VERSION 3.12)
project(chat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL include dirs: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libs: ${OPENSSL_LIBRARIES}")

add_executable(chat socket_chat.cpp)

target_include_directories(chat PRIVATE
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(chat PRIVATE
    OpenSSL::SSL
)
