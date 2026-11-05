cmake_minimum_required(VERSION 3.21)

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

# Compile-only scaffold: unfinished methods are intentionally not linked into the demo.
add_library(trading_skeleton OBJECT
    src/Bar.cpp
    src/Order.cpp
    src/Fill.cpp
    src/CsvDataFeed.cpp
    src/Strategy.cpp
    src/MovingAverageStrategy.cpp
    src/Broker.cpp
    src/BrokerSim.cpp
    src/OrderManager.cpp
    src/FillProcessor.cpp
    src/RiskManager.cpp
    src/PerformanceReport.cpp
    src/Backtester.cpp
    src/EventJournal.cpp
    src/IbkrBroker.cpp
    src/TradingApp.cpp
)
target_include_directories(trading_skeleton PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(trading_skeleton PRIVATE cxx_std_23)
target_compile_options(trading_skeleton PRIVATE -Wall -Wextra -Wpedantic)

# Compile every public header independently to catch missing direct includes.
file(GLOB public_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
foreach(header IN LISTS public_headers)
    get_filename_component(header_name "${header}" NAME)
    set(check_source "${CMAKE_CURRENT_BINARY_DIR}/header_${header_name}.cpp")
    file(WRITE "${check_source}" "#include \"${header_name}\"\n")
    target_sources(trading_skeleton PRIVATE "${check_source}")
endforeach()

add_test(NAME portfolio_demo
    COMMAND ${CMAKE_COMMAND} -DDEMO=$<TARGET_FILE:mainmain>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_demo.cmake)
