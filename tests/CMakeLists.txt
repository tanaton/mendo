add_executable(mendo_tests
    test_document.cpp
    test_document_service.cpp
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
    test_mermaid_util.cpp
    test_mermaid_file_cache.cpp
    test_ini_parser.cpp
    test_config_store.cpp
    test_viewport_manager.cpp
    test_text_measurer.cpp
    test_draw_command.cpp
    test_app_controller.cpp
    test_pane_controller.cpp
    test_nav_history.cpp
    test_navigation_service.cpp
    test_nav_button_format.cpp
    test_mouse_gesture.cpp
    test_theme_service.cpp
    test_file_load_service.cpp
    test_layout_cache.cpp
    test_copy_button.cpp
    test_swipe_detector.cpp
    test_titlebar.cpp
    test_ui_constants.cpp
    test_toast_notifier.cpp
    test_context_menu.cpp
    test_image.cpp
    test_help.cpp
    test_search_state.cpp
    test_locale.cpp
)

target_link_libraries(mendo_tests PRIVATE
    mendo_core
    GTest::gtest_main
)
target_compile_definitions(mendo_tests PRIVATE MENDO_TESTING)

if(MSVC)
    target_compile_options(mendo_tests PRIVATE /W4 /utf-8)
endif()

include(GoogleTest)
gtest_add_tests(TARGET mendo_tests)
