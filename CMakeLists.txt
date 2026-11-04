cmake_minimum_required(VERSION 4.2)
project(mendo LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 単一exeで配布するためにCRTを静的リンク
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- MSVCビルド最適化 ----
# /MP: マルチプロセッサコンパイル（各ターゲット内で並列コンパイル）
add_compile_options(/MP)

# /GL: プログラム全体の最適化（コンパイル時にLTCGを有効化）
# /Gy: 関数レベルリンク（リンカが未使用関数を除去可能にする）
# /Gw: グローバルデータも COMDAT 化し、/OPT:REF で未使用分を除去させる
add_compile_options($<$<CONFIG:Release>:/GL> $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)

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

# ---- Tracy プロファイラ（オプション） ----
# 開発時のフレーム/ゾーン計測用。Release 配布では OFF が前提。
# ON にすると <tracy/Tracy.hpp> が <windows.h> を引き込むため、
# Tracy の zone/frame マクロは profiler.h 経由でラップして使う。
option(MENDO_USE_TRACY "Build with Tracy profiler client" OFF)

include(FetchContent)
if(MENDO_USE_TRACY)
    # Tracy v0.14.1 のソース取得（add_subdirectory 相当）。
    # TRACY_ENABLE はクライアント側計装の有効化スイッチで、Tracy の CMakeLists
    # 内で TracyClient のコンパイル定義として伝播する。
    # TRACY_ON_DEMAND は Tracy GUI が接続するまで計測バッファを溜めない設定。
    # アプリ起動だけでメモリを食わないので開発用に都合がよい。
    set(TRACY_ENABLE    ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_STATIC    ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        tracy
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.tar.gz
    )
    FetchContent_MakeAvailable(tracy)
    # CRT 設定をプロジェクト全体と揃える（/MT[d]）。
    if(TARGET TracyClient)
        set_target_properties(TracyClient PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(TracyClient PRIVATE /W0)
    endif()
endif()

# ---- QuickJS-ng（pele と KaTeX を動かす） ----
# ライブラリの 4 ファイルだけを組む。SOURCE_SUBDIR を無い場所にして、同梱の CMakeLists (qjs などの実行ファイル) を読ませない。
FetchContent_Declare(
    quickjs
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.18.0.tar.gz
    URL_HASH SHA256=28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _none
)
FetchContent_MakeAvailable(quickjs)
add_library(quickjs STATIC
    ${quickjs_SOURCE_DIR}/quickjs.c
    ${quickjs_SOURCE_DIR}/libregexp.c
    ${quickjs_SOURCE_DIR}/libunicode.c
    ${quickjs_SOURCE_DIR}/dtoa.c
)
target_include_directories(quickjs SYSTEM PUBLIC ${quickjs_SOURCE_DIR})
# 同梱の CMakeLists と同じ定義と C11 (Windows の原子操作は /experimental:c11atomics)。
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(quickjs PRIVATE /W0 /experimental:c11atomics)

# pele と KaTeX はビルドの時にバイトコードにして埋め込む (実行時のコンパイル約 60ms と約 20ms を省く)。
# 同じ quickjs ターゲットで組むので、バイトコードの版は実行時のものと必ず合う。
add_executable(mendo_js_compile src/mermaid/js_compile.cpp)
target_link_libraries(mendo_js_compile PRIVATE quickjs)
target_compile_options(mendo_js_compile PRIVATE /W4 /utf-8)
set(MENDO_JS_BYTECODE_DIR "${CMAKE_BINARY_DIR}/js_bytecode")
set(MENDO_JS_BYTECODE "${MENDO_JS_BYTECODE_DIR}/pele.qbc" "${MENDO_JS_BYTECODE_DIR}/katex.qbc")
add_custom_command(
    OUTPUT ${MENDO_JS_BYTECODE}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MENDO_JS_BYTECODE_DIR}"
    COMMAND mendo_js_compile "${CMAKE_SOURCE_DIR}/third_party/pele/pele.mjs" "${MENDO_JS_BYTECODE_DIR}/pele.qbc" module
    COMMAND mendo_js_compile "${CMAKE_SOURCE_DIR}/third_party/katex/katex.min.js" "${MENDO_JS_BYTECODE_DIR}/katex.qbc" global
    DEPENDS mendo_js_compile "${CMAKE_SOURCE_DIR}/third_party/pele/pele.mjs" "${CMAKE_SOURCE_DIR}/third_party/katex/katex.min.js"
    VERBATIM
)
add_custom_target(mendo_js_bytecode DEPENDS ${MENDO_JS_BYTECODE})

# ---- md4c（サードパーティ） ----
# Document テキスト表現は UTF-8 (char) 固定。md4c は MD_CHAR=char で動作する。
add_library(md4c STATIC third_party/md4c/md4c.c)
target_include_directories(md4c PUBLIC third_party/md4c)
target_compile_options(md4c PRIVATE /W0)

# ---- サブディレクトリ定義 ----
set(MENDO_SRC_DIRS
    src src/app src/window src/core src/render src/layout
    src/ui src/input src/io src/mermaid src/nav src/theme src/util
)

# ---- コアライブラリ（テストがリンクするコード。WinMain・App は含まない） ----
add_library(mendo_core STATIC
    # core - ドキュメントモデル・パース
    src/core/document.cpp
    src/core/document_utils.cpp
    src/core/anchor.cpp
    src/core/parser.cpp
    src/core/html_entities.cpp
    src/core/selection_html.cpp
    src/core/syntax.cpp
    # layout - レイアウト計算
    src/layout/layout_cache.cpp
    src/layout/layout.cpp
    src/layout/layout_computer.cpp
    src/layout/dwrite_measurer.cpp
    src/layout/inline_math.cpp
    src/layout/doc_dwrite_bridge.cpp
    # render - 描画パイプライン
    src/render/command_generator.cpp
    src/render/command_executor.cpp
    # ui - UIコンポーネント
    src/ui/context_menu.cpp
    src/ui/search_state.cpp
    src/ui/settings_dialog.cpp
    src/ui/titlebar.cpp
    src/ui/tooltip.cpp
    # input - 入力処理
    src/input/hit_test_service.cpp
    # io - ファイルI/O・永続化
    src/io/file_explorer.cpp
    src/io/config_service.cpp
    src/io/image_loader.cpp
    src/io/file_watcher.cpp
    # mermaid - Mermaid図表サポート
    src/mermaid/mermaid_util.cpp
    src/mermaid/mermaid_file_cache.cpp
    src/mermaid/diagram_raster.cpp
    src/mermaid/pele_engine.cpp
    src/mermaid/diagram_view.cpp
    # nav - ナビゲーション
    src/nav/nav.cpp
    # theme - テーマ管理
    src/theme/theme.cpp
    # app - アプリケーション制御
    src/app/async_load_coordinator.cpp
    src/app/app_controller.cpp
    src/app/clipboard_manager.cpp
    src/app/reducer.cpp
    src/app/resource_manager.cpp
    src/app/startup_plan.cpp
    src/app/theme_service.cpp
    # util - ユーティリティ
    src/util/file_io.cpp
    src/util/pane_layout.cpp
    src/util/task_scheduler.cpp
    src/util/win32_util.cpp
)
target_include_directories(mendo_core PUBLIC ${MENDO_SRC_DIRS})
target_compile_definitions(mendo_core PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
# リンクする OS のライブラリはここにまとめる (ソースの #pragma comment(lib) は使わない)。
# windowscodecs は DLL を静的に読まないが、WIC の GUID がこの lib にある。
target_link_libraries(mendo_core PUBLIC
    md4c quickjs d2d1 dwrite windowscodecs dwmapi uxtheme ole32 shell32 comdlg32
)
if(MENDO_USE_TRACY)
    # profiler.h のマクロを Tracy 呼び出しに展開するスイッチ。TRACY_ENABLE は TracyClient から伝わる。
    target_compile_definitions(mendo_core PUBLIC MENDO_USE_TRACY)
    target_link_libraries(mendo_core PUBLIC Tracy::TracyClient)
endif()
# /Zc:preprocessor: C++20 準拠 preprocessor (__VA_OPT__ 等)。
# profiler.h の MENDO_LOGF/STATF/TRACEF が __VA_OPT__ を使い、利用側 TU にも展開されるため
# PUBLIC で伝播させる (依存先の Tracy/md4c には伝播しない)。
target_compile_options(mendo_core PUBLIC /W4 /utf-8 /Zc:preprocessor)

# ---- プリコンパイル済みヘッダー ----
# `<pch.h>` 形式で指定するのは、CMake が生成する wrapper に絶対パスを書かれると
# 日本語パス配下では MSVC `/utf-8` と衝突するため。
target_precompile_headers(mendo_core PRIVATE <pch.h>)

# ---- メイン実行ファイル ----
add_executable(mendo WIN32
    src/main.cpp
    src/window/window.cpp
    src/app/app.cpp
    src/render/renderer.cpp
    src/render/renderer_device.cpp
    src/mermaid/mermaid.cpp
    res/mendo.rc
)
target_link_libraries(mendo PRIVATE mendo_core comctl32 d3d11 imm32 delayimp)
add_dependencies(mendo mendo_js_bytecode)
set_source_files_properties(res/mendo.rc PROPERTIES OBJECT_DEPENDS "${MENDO_JS_BYTECODE}" INCLUDE_DIRECTORIES "${MENDO_JS_BYTECODE_DIR}")
# ファイルダイアログでしか使わないので、起動時にロードしない (約 1ms)。
# comctl32 は検索の EDIT とツールチップを初めて作るときまで要らない。
# shell32 はドロップ・外部で開く・設定フォルダの取得の予備でしか使わない。
target_link_options(mendo PRIVATE /DELAYLOAD:comdlg32.dll /DELAYLOAD:comctl32.dll /DELAYLOAD:shell32.dll)
# KnownDLLs に無い DLL を静的インポートすると、ローダーが exe と同じフォルダを先に探すので
# Downloads 等に置かれた偽 DLL を読んでしまう。遅延ロードにして、wWinMain 先頭の
# SetDefaultDllDirectories (System32 限定) の後で解決させる。
target_link_options(mendo PRIVATE
    /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dwmapi.dll /DELAYLOAD:uxtheme.dll
    /DEPENDENTLOADFLAG:0x800
)
# 自動生成マニフェストを無効化（RC経由で独自のマニフェストを提供するため）
target_link_options(mendo PRIVATE /MANIFEST:NO)

target_precompile_headers(mendo REUSE_FROM mendo_core)

# ---- アセンブラリスト出力（オプション） ----
# /GL 有効時はリストをリンカが LTCG 後のコードで書き出す。出力先はコンパイル時に
# 埋め込まれるため、mendo_core をリンクする mendo_tests も同じファイルを上書きする。
# mendo の結果だけ欲しいときは --target mendo でビルドする。
option(MENDO_ASM_LISTING "Emit source-annotated assembly listings (/FAs)" OFF)
if(MENDO_ASM_LISTING)
    foreach(_mendo_target mendo_core mendo)
        target_compile_options(${_mendo_target} PRIVATE
            /FAs "/Fa${CMAKE_BINARY_DIR}/asm/$<CONFIG>/")
    endforeach()
    unset(_mendo_target)
endif()

# ---- テスト ----
option(MENDO_BUILD_TESTS "Build tests" ON)
if(MENDO_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
    )
    # CRT は CMAKE_MSVC_RUNTIME_LIBRARY を定義してあるので、gtest も静的リンク (/MT) で組まれる。
    # gmock は使っていない。
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()

# ---- ベンチマーク ----
# tests/ のヘルパーとモックを使う。それらは gtest を include するので、テストのビルドが要る。
option(MENDO_BUILD_BENCHMARKS "Build benchmarks" OFF)
if(MENDO_BUILD_BENCHMARKS)
    if(NOT MENDO_BUILD_TESTS)
        message(FATAL_ERROR "mendo: MENDO_BUILD_BENCHMARKS requires MENDO_BUILD_TESTS.")
    endif()
    FetchContent_Declare(
        benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.10.0.zip
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
    add_subdirectory(bench)
endif()
