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
    test_mermaid_lifecycle.cpp
    test_ini_parser.cpp
    test_config_service.cpp
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
    test_hit_test_service.cpp
    test_hit_test_service_dwrite.cpp
    test_swipe_detector.cpp
    test_titlebar.cpp
    test_ui_constants.cpp
    test_toast_notifier.cpp
    test_context_menu.cpp
    test_image.cpp
    test_help.cpp
    test_search_state.cpp
    test_locale.cpp
    test_memory_eviction.cpp
    test_lru_cache.cpp
    test_scroll_restoration.cpp
    test_hover_throttle.cpp
    test_search_bar_controller.cpp
    test_session_service.cpp
    test_reducer.cpp
    test_string_convert.cpp
    test_render_composer.cpp
    test_file_io.cpp
    test_utility.cpp
    test_tooltip_target.cpp
    test_task_scheduler.cpp
    test_side_effect_executor.cpp
    test_command_generator_frame.cpp
    test_command_generator_content.cpp
    test_command_generator_highlight.cpp
    test_measurer_parity.cpp
    test_command_executor.cpp
    test_resource_manager.cpp
    test_ascii_util.cpp
    test_html_entity.cpp
    test_small_vector.cpp
    test_pmr_unique_ptr.cpp
    test_fenwick.cpp
    test_dirty_scheduler.cpp
)

target_link_libraries(mendo_tests PRIVATE
    mendo_core
    GTest::gtest_main
)
target_compile_definitions(mendo_tests PRIVATE MENDO_TESTING)

if(MSVC)
    target_compile_options(mendo_tests PRIVATE /W4 /utf-8)
endif()

# ---- プリコンパイル済みヘッダー ----
# `<test_pch.h>` 形式は日本語パス + `/utf-8` 衝突回避のため。tests/ を include path に追加する必要がある。
target_include_directories(mendo_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_precompile_headers(mendo_tests PRIVATE <test_pch.h>)
