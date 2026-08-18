cmake_minimum_required(VERSION 3.20)
project(quiescesim_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(quiescesim_native src/engine.cpp src/frontend.cpp src/ibex_counter.cpp src/ir.cpp)
target_include_directories(quiescesim_native PUBLIC include)
target_compile_options(quiescesim_native PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(quiescesim_native_tests tests/engine_tests.cpp)
target_link_libraries(quiescesim_native_tests PRIVATE quiescesim_native)

add_executable(quiescesim_run src/main.cpp)
target_link_libraries(quiescesim_run PRIVATE quiescesim_native)

add_executable(quiescesim_ibex_counter64_run src/ibex_counter_main.cpp)
target_link_libraries(quiescesim_ibex_counter64_run PRIVATE quiescesim_native)

enable_testing()
add_test(NAME quiescesim_native_tests COMMAND quiescesim_native_tests)
