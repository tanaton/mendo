add_executable(MaDView_tests
    test_parser.cpp
    test_types.cpp
    test_theme.cpp
    test_file_loader.cpp
    test_file_explorer.cpp
    test_toc.cpp
    test_layout.cpp
    test_anchor.cpp
    test_syntax.cpp
    test_document_utils.cpp
    test_pane_layout.cpp
)

target_link_libraries(MaDView_tests PRIVATE
    MaDView_core
    GTest::gtest_main
)

if(MSVC)
    target_compile_options(MaDView_tests PRIVATE /W4 /utf-8)
endif()

include(GoogleTest)
gtest_add_tests(TARGET MaDView_tests)
