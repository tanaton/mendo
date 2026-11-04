# tests/ の test_*.cpp をすべて入れる。足したファイルも cmake をやり直さずにビルドへ入る (CONFIGURE_DEPENDS)。
file(GLOB _mendo_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
add_executable(mendo_tests ${_mendo_test_sources})
unset(_mendo_test_sources)

# tests/ のヘルパーを使うターゲット (mendo_tests / mendo_bench) 共通の設定。
add_library(mendo_test_support INTERFACE)
target_include_directories(mendo_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# cwd に依存せず example/ を開けるよう、ソースツリーの絶対パスを渡す。
# u8 リテラルで定義し、std::filesystem::path(const char8_t*) に直接渡せるようにする (UTF-8 保証)。
target_compile_definitions(mendo_test_support INTERFACE "MENDO_EXAMPLE_DIR=u8\"${CMAKE_SOURCE_DIR}/example\"")

target_link_libraries(mendo_tests PRIVATE
    mendo_core
    mendo_test_support
    GTest::gtest_main
    shlwapi
)
# テストの変更のたびに全体の LTCG を待たない (1 ファイルの修正で 34 → 11 秒)。
target_link_options(mendo_tests PRIVATE $<$<CONFIG:Release>:/LTCG:INCREMENTAL>)
# test_help が埋め込みヘルプ (res/help_*.md) の実物を読むため。
target_compile_definitions(mendo_tests PRIVATE "MENDO_RES_DIR=u8\"${CMAKE_SOURCE_DIR}/res\"")
# test_pele_engine が KaTeX の書体の実物と、ビルドで作った pele と KaTeX のバイトコードを読むため。
target_compile_definitions(mendo_tests PRIVATE "MENDO_THIRD_PARTY_DIR=u8\"${CMAKE_SOURCE_DIR}/third_party\""
                                               "MENDO_JS_BYTECODE_DIR=u8\"${MENDO_JS_BYTECODE_DIR}\"")
add_dependencies(mendo_tests mendo_js_bytecode)

# ---- プリコンパイル済みヘッダー ----
# `<test_pch.h>` 形式は日本語パス + `/utf-8` 衝突回避のため。tests/ は mendo_test_support で include path に入る。
target_precompile_headers(mendo_tests PRIVATE <test_pch.h>)
