cmake_minimum_required(VERSION 3.16)
project(rockidserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)

find_path(LIBPQ_INCLUDE_DIR libpq-fe.h)
if(NOT LIBPQ_INCLUDE_DIR)
  set(LIBPQ_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party)
  message(STATUS "libpq-fe.h not found, using ${LIBPQ_INCLUDE_DIR}/libpq-fe.h")
endif()

find_library(LIBPQ_LIBRARY pq)
if(NOT LIBPQ_LIBRARY)
  find_library(LIBPQ_LIBRARY NAMES libpq.so.5 pq.so.5
    PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu)
endif()
if(NOT LIBPQ_LIBRARY)
  message(FATAL_ERROR "libpq not found")
endif()

add_executable(rockidserver
  src/main.cpp
  src/log.cpp
  src/json.cpp
  src/database.cpp
  src/database_extra.cpp
  src/connection.cpp
  src/server.cpp
  src/app.cpp
  src/app_features.cpp
)

target_include_directories(rockidserver PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LIBPQ_INCLUDE_DIR}
)

target_link_libraries(rockidserver PRIVATE
  OpenSSL::Crypto
  ${LIBPQ_LIBRARY}
  pthread
)

target_compile_options(rockidserver PRIVATE -Wall -Wextra -Wpedantic)
