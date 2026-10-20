cmake_minimum_required(VERSION 3.16)
project(rockidserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)

find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
if(NOT SQLITE3_INCLUDE_DIR)
  set(SQLITE3_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party)
  message(STATUS "sqlite3.h not found, using ${SQLITE3_INCLUDE_DIR}/sqlite3.h")
endif()

find_library(SQLITE3_LIBRARY sqlite3)
if(NOT SQLITE3_LIBRARY)
  find_library(SQLITE3_LIBRARY NAMES sqlite3.so.0 libsqlite3.so.0
    PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
endif()
if(NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "libsqlite3 not found")
endif()

add_executable(rockidserver
  src/main.cpp
  src/log.cpp
  src/json.cpp
  src/database.cpp
  src/connection.cpp
  src/server.cpp
  src/app.cpp
)

target_include_directories(rockidserver PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${SQLITE3_INCLUDE_DIR}
)

target_link_libraries(rockidserver PRIVATE
  OpenSSL::Crypto
  ${SQLITE3_LIBRARY}
  pthread
)

target_compile_options(rockidserver PRIVATE -Wall -Wextra -Wpedantic)
