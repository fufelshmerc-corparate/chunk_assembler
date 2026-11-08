cmake_minimum_required(VERSION 3.14)
project(chunk_assembler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Предупреждения и санитайзеры
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

if(ENABLE_TSAN AND ENABLE_ASAN)
  message(FATAL_ERROR "TSan and ASan cannot be combined")
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# Библиотеки
add_library(interval_set INTERFACE)
target_include_directories(interval_set INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(chunk_assembler src/mapped_file.cpp src/chunk_assembler.cpp)
target_link_libraries(chunk_assembler PUBLIC interval_set Threads::Threads)

# Пример
add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE chunk_assembler)

# Тесты
include(CTest)
if(BUILD_TESTING)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip)
    FetchContent_MakeAvailable(googletest)
  endif()

  include(GoogleTest)

  add_executable(interval_set_test tests/interval_set_test.cpp)
  target_link_libraries(interval_set_test PRIVATE interval_set GTest::gtest_main)
  gtest_discover_tests(interval_set_test)

  foreach(name mapped_file_test chunk_assembler_test chunk_assembler_mt_test)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE chunk_assembler GTest::gtest_main)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
  endforeach()
endif()
