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
    src/market_maker.cpp
    src/analytics.cpp
    src/mm_sim.cpp
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
    tests/test_market_maker.cpp
    tests/test_analytics.cpp
    tests/test_mm_sim.cpp
    ${CORE_SOURCES}
)
target_include_directories(orderbook_tests PRIVATE src tests)

enable_testing()
# Run from the source root so tests that use repo-relative paths (e.g. the
# analytics CSV written to build/) resolve the same way under ctest as they do
# when orderbook_tests is invoked directly from the project root.
add_test(NAME all_tests COMMAND orderbook_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
