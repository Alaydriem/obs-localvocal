cmake_minimum_required(VERSION 3.28)
project(teb-caption-sender-test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BuildMyCurl.cmake picks the prebuilt libcurl from CMAKE_BUILD_TYPE; this project
# always builds Release.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/BuildMyCurl.cmake)

find_package(Threads REQUIRED)

enable_testing()

add_executable(
  teb-caption-sender-test teb-caption-sender-test.cpp local-http-listener.cpp
                          ${CMAKE_CURRENT_SOURCE_DIR}/../../teb-caption-sender.cpp)
target_include_directories(teb-caption-sender-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(teb-caption-sender-test PRIVATE libcurl Threads::Threads)
if(WIN32)
  target_link_libraries(teb-caption-sender-test PRIVATE ws2_32)
endif()

add_test(NAME teb-caption-sender COMMAND teb-caption-sender-test)
