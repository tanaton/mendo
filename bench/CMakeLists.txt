add_executable(mendo_bench benchmarks.cpp)
target_link_libraries(mendo_bench PRIVATE
    mendo_core
    mendo_test_support
    benchmark::benchmark_main
    GTest::gtest
)
