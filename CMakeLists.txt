cmake_minimum_required(VERSION 3.16)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(CORE_SOURCES
    src/order_book.cpp
    src/matching_engine.cpp
    src/display.cpp
    src/simulator.cpp
    src/repl.cpp
    src/benchmark.cpp
    src/flow.cpp
)

add_executable(orderbook src/main.cpp ${CORE_SOURCES})
target_include_directories(orderbook PRIVATE src)

add_executable(orderbook_tests
    tests/test_main.cpp
    tests/test_order_book.cpp
    tests/test_matching.cpp
    tests/test_display.cpp
    tests/test_simulator.cpp
    tests/test_repl.cpp
    tests/test_flow.cpp
    ${CORE_SOURCES}
)
target_include_directories(orderbook_tests PRIVATE src tests)

enable_testing()
add_test(NAME all_tests COMMAND orderbook_tests)
