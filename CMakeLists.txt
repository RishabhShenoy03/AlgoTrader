cmake_minimum_required(VERSION 3.16)

project(AlgoTrader LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

enable_testing()

add_library(event_core
    src/Portfolio.cpp
    src/free_functions.cpp
)

target_compile_features(event_core PUBLIC cxx_std_23)

target_compile_options(event_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_include_directories(event_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(mainmain
    src/main.cpp
)

target_link_libraries(mainmain PRIVATE
    event_core
)