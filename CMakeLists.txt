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

# 展開されたWebView2のヘッダーとライブラリのパスを設定。
# NuGet パッケージは build/native/x64 と build/native/arm64 のみ提供。
# Visual Studio ジェネレータでは CMAKE_GENERATOR_PLATFORM、それ以外は
# CMAKE_SYSTEM_PROCESSOR を見て切り替える。
set(WEBVIEW2_DIR "${webview2_SOURCE_DIR}")
set(WEBVIEW2_INCLUDE "${WEBVIEW2_DIR}/build/native/include")
if(CMAKE_GENERATOR_PLATFORM)
    set(_mendo_arch "${CMAKE_GENERATOR_PLATFORM}")
else()
    set(_mendo_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_mendo_arch}" _mendo_arch)
if(_mendo_arch MATCHES "^(x64|amd64)$")
    set(WEBVIEW2_LIB_DIR "${WEBVIEW2_DIR}/build/native/x64")
elseif(_mendo_arch MATCHES "^(arm64|aarch64)$")
    set(WEBVIEW2_LIB_DIR "${WEBVIEW2_DIR}/build/native/arm64")
else()
    message(FATAL_ERROR
        "mendo: unsupported architecture '${_mendo_arch}'. "
        "WebView2 NuGet provides only x64 / arm64 binaries "
        "(accepted names: x64, AMD64, ARM64, AArch64). "
        "Visual Studio generator users should pass '-A x64' or '-A ARM64' "
        "(e.g. 'cmake -A x64 -B build').")
endif()
unset(_mendo_arch)

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
# UTF-16 入力モードを有効化（MD_CHAR=WCHAR）。
# md4c.h を取り込む側（mendo_core 等）にも伝播させるため PUBLIC で定義。
target_compile_definitions(md4c PUBLIC MD4C_USE_UTF16)
if(MSVC)
    target_compile_options(md4c PRIVATE /W0)
    set_target_properties(md4c PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- サブディレクトリ定義 ----
set(MENDO_SRC_DIRS
    src src/app src/window src/core src/render src/layout
    src/ui src/input src/io src/mermaid src/nav src/theme src/util
)

# ---- コアライブラリ（テスト可能なコード、WinMain/ウィンドウなし） ----
add_library(mendo_core STATIC
    # core - ドキュメントモデル・パース
    src/core/document.cpp
    src/core/document_service.cpp
    src/core/document_utils.cpp
    src/core/parser.cpp
    src/core/reload.cpp
    src/core/selection_html.cpp
    src/core/toc.cpp
    src/core/syntax.cpp
    # layout - レイアウト計算
    src/layout/layout.cpp
    src/layout/dwrite_measurer.cpp
    # render - 描画パイプライン
    src/render/command_generator.cpp
    src/render/command_executor.cpp
    # app - 描画ステート構築（Win32/D2D 非依存の純粋ロジック）
    src/app/render_composer.cpp
    # app - 副作用実行（Win32 API は IWin32Host 経由で隔離）
    src/app/side_effect_executor.cpp
    # app - 画像/Mermaid リソース管理（Mermaid 具象は IMermaidRenderer 経由で隔離）
    src/app/resource_manager.cpp
    # ui - UIコンポーネント
    # context_menu.cpp は Win32/D2D 描画層（本体）と logic 層（mendo_core 入り）に
    # 分割されており、logic 側のみを mendo_core に含める。
    src/ui/context_menu_logic.cpp
    src/ui/pane_layout.cpp
    src/ui/pane_controller.cpp
    src/ui/search_state.cpp
    src/ui/search_bar_controller.cpp
    src/ui/titlebar.cpp
    src/ui/tooltip.cpp
    # input - 入力処理
    src/input/hit_test_service.cpp
    # io - ファイルI/O・永続化
    src/io/file_loader.cpp
    src/io/file_explorer.cpp
    src/io/file_load_service.cpp
    src/io/config_service.cpp
    src/io/image_loader.cpp
    src/io/file_watcher.cpp
    # mermaid - Mermaid図表サポート
    src/mermaid/mermaid_util.cpp
    src/mermaid/mermaid_file_cache.cpp
    # nav - ナビゲーション
    src/nav/nav.cpp
    # theme - テーマ管理
    src/theme/theme.cpp
    src/theme/theme_service.cpp
    # app - アプリケーション制御
    src/app/app_controller.cpp
    src/app/reducer.cpp
    # util - ユーティリティ
    src/util/task_scheduler.cpp
)
target_include_directories(mendo_core PUBLIC ${MENDO_SRC_DIRS} third_party/md4c)
target_compile_definitions(mendo_core PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    $<$<CONFIG:Release>:MENDO_PROFILE_ENABLED=0>
)
target_link_libraries(mendo_core PUBLIC
    md4c d2d1 dwrite windowscodecs shlwapi comctl32
)
if(MSVC)
    target_compile_options(mendo_core PRIVATE /W4 /utf-8)
endif()

# ---- プリコンパイル済みヘッダー ----
# `<pch.h>` 形式で指定するのは、CMake が生成する wrapper に絶対パスを書かれると
# 日本語パス配下では MSVC `/utf-8` と衝突するため。
target_precompile_headers(mendo_core PRIVATE <pch.h>)

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
    src/window/window.cpp
    src/app/app.cpp
    src/app/app_init.cpp
    src/app/app_file.cpp
    src/app/app_scroll.cpp
    src/app/app_mouse.cpp
    src/app/app_mouse_click.cpp
    src/app/app_mouse_hover.cpp
    src/app/app_navigate.cpp
    src/app/app_context_menu.cpp
    src/app/app_clipboard.cpp
    src/app/win32_host_impl.cpp
    src/ui/context_menu.cpp
    src/render/renderer.cpp
    src/render/renderer_resources.cpp
    src/render/renderer_pane.cpp
    src/render/renderer_titlebar.cpp
    src/render/renderer_overlay.cpp
    src/render/renderer_search.cpp
    src/render/d2d_render_backend.cpp
    src/mermaid/mermaid.cpp
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

target_precompile_headers(mendo REUSE_FROM mendo_core)

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
