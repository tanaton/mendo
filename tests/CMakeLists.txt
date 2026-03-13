add_executable(mdviewer_tests
    test_parser.cpp
    test_types.cpp
    test_theme.cpp
    test_file_loader.cpp
    test_layout.cpp
    test_anchor.cpp
)

target_link_libraries(mdviewer_tests PRIVATE
    mdviewer_core
    GTest::gtest_main
)

if(MSVC)
    target_compile_options(mdviewer_tests PRIVATE /W4 /utf-8)
endif()

include(GoogleTest)
gtest_discover_tests(mdviewer_tests)
