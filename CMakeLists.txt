cmake_minimum_required(VERSION 3.16)

project(AlgoTrader LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

enable_testing()

add_library(event_core
    src/Portfolio.cpp
)

target_compile_features(event_core PUBLIC cxx_std_23)

target_compile_options(event_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(event_gateway
    src/main.cpp
)

target_link_libraries(event_gateway PRIVATE
    event_core
)