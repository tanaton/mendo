cmake_minimum_required(VERSION 3.20)
project(mendo LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 単一exeで配布するためにCRTを静的リンク
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- MSVCビルド最適化 ----
if(MSVC)
    # /MP: マルチプロセッサコンパイル（各ターゲット内で並列コンパイル）
    add_compile_options(/MP)

    # /GL: プログラム全体の最適化（コンパイル時にLTCGを有効化）
    # /Gy: 関数レベルリンク（リンカが未使用関数を除去可能にする）
    add_compile_options($<$<CONFIG:Release>:/GL> $<$<CONFIG:Release>:/Gy>)

    # /LTCG: リンク時コード生成
    # /OPT:REF: 未参照の関数/データを除去
    # /OPT:ICF: 同一COMDATの統合
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )

    # 静的ライブラリにもリンカが処理できるよう/LTCGが必要
    set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
endif()

# ---- WebView2 SDK（Mermaid図表レンダリング用） ----
include(FetchContent)
FetchContent_Declare(
    webview2
    URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
    DOWNLOAD_NO_EXTRACT FALSE
)
FetchContent_MakeAvailable(webview2)

# 展開されたWebView2のヘッダーとライブラリのパスを設定
set(WEBVIEW2_DIR "${webview2_SOURCE_DIR}")
set(WEBVIEW2_INCLUDE "${WEBVIEW2_DIR}/build/native/include")
set(WEBVIEW2_LIB_DIR "${WEBVIEW2_DIR}/build/native/x64")

# WIL（Windows Implementation Library）WebView2コールバック用
FetchContent_Declare(
    wil
    URL https://www.nuget.org/api/v2/package/Microsoft.Windows.ImplementationLibrary/1.0.240803.1
    DOWNLOAD_NO_EXTRACT FALSE
)
FetchContent_MakeAvailable(wil)
set(WIL_INCLUDE "${wil_SOURCE_DIR}/include")

# ---- md4c（サードパーティ） ----
add_library(md4c STATIC third_party/md4c/md4c.c)
target_include_directories(md4c PUBLIC third_party/md4c)
if(MSVC)
    target_compile_options(md4c PRIVATE /W0)
    set_target_properties(md4c PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- コアライブラリ（テスト可能なコード、WinMain/ウィンドウなし） ----
add_library(mendo_core STATIC
    src/document.cpp
    src/document_service.cpp
    src/hit_test_service.cpp
    src/parser.cpp
    src/layout.cpp
    src/layout_service.cpp
    src/dwrite_measurer.cpp
    src/command_generator.cpp
    src/command_executor.cpp
    src/file_loader.cpp
    src/theme.cpp
    src/syntax.cpp
    src/file_explorer.cpp
    src/toc.cpp
    src/document_utils.cpp
    src/pane_layout.cpp
    src/mermaid_util.cpp
    src/mermaid_file_cache.cpp
    src/config_store.cpp
    src/app_controller.cpp
    src/pane_controller.cpp
    src/nav_history.cpp
    src/navigation_service.cpp
    src/theme_service.cpp
    src/file_load_service.cpp
    src/context_menu.cpp
    src/image_loader.cpp
    src/task_scheduler.cpp
    src/search_state.cpp
)
target_include_directories(mendo_core PUBLIC src third_party/md4c)
target_compile_definitions(mendo_core PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(mendo_core PUBLIC
    md4c d2d1 dwrite windowscodecs shlwapi comctl32
)
if(MSVC)
    target_compile_options(mendo_core PRIVATE /W4 /utf-8)
endif()

# ---- mermaid.min.jsをgzip圧縮してexeサイズを縮小 ----
set(MERMAID_JS_SRC "${CMAKE_SOURCE_DIR}/third_party/mermaid/mermaid.min.js")
set(MERMAID_JS_GZ  "${CMAKE_SOURCE_DIR}/third_party/mermaid/mermaid.min.js.gz")
add_custom_command(
    OUTPUT  "${MERMAID_JS_GZ}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_SOURCE_DIR}/cmake/gzip.ps1"
        "${MERMAID_JS_SRC}" "${MERMAID_JS_GZ}"
    DEPENDS "${MERMAID_JS_SRC}"
    COMMENT "Compressing mermaid.min.js with gzip"
    VERBATIM
)
add_custom_target(mermaid_gz DEPENDS "${MERMAID_JS_GZ}")

# ---- メイン実行ファイル ----
add_executable(mendo WIN32
    src/main.cpp
    src/window.cpp
    src/app.cpp
    src/app_scroll.cpp
    src/app_mouse.cpp
    src/app_navigate.cpp
    src/renderer.cpp
    src/renderer_pane.cpp
    src/d2d_render_backend.cpp
    src/mermaid.cpp
    res/mendo.rc
)
add_dependencies(mendo mermaid_gz)
target_include_directories(mendo PRIVATE ${WEBVIEW2_INCLUDE} ${WIL_INCLUDE})
target_link_libraries(mendo PRIVATE mendo_core
    "${WEBVIEW2_LIB_DIR}/WebView2LoaderStatic.lib"
)
if(MSVC)
    target_compile_options(mendo PRIVATE /W4 /utf-8)
endif()
# 自動生成マニフェストを無効化（RC経由で独自のマニフェストを提供するため）
set_target_properties(mendo PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# ---- テスト ----
option(MENDO_BUILD_TESTS "Build tests" ON)
if(MENDO_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    )
    # GoogleTestがCRT設定を上書きするのを防止
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()
