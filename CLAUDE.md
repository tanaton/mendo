# mendo - 高速＆省メモリMarkdownビュアー

## プロジェクト概要

Direct2D/DirectWriteを使用して直接描画を行うMarkdownビュアー。

## 技術スタック

- **言語**: C++23 (MSVC)
- **GUI**: Win32 API (`WNDCLASSEXW` + メッセージループ)
- **描画**: Direct2D (`ID2D1Factory1` + `ID2D1DeviceContext`)
- **テキスト**: DirectWrite (`IDWriteTextLayout`)
- **Markdownパーサ**: md4c (SAX型コールバック、`third_party/md4c/`)
- **テスト**: Google Test v1.18.0
- **ベンチマーク**: Google Benchmark v1.10.0 (`bench/`、`-DMENDO_BUILD_BENCHMARKS=ON` のときだけ)
- **ビルド**: CMake 4.2+ (Visual Studio 2026)

## ビルド方法

```bash
cmake -B build
cmake --build build --config Release -- //v:q //nologo
```

## テスト実行

```bash
build/tests/Release/mendo_tests.exe --gtest_brief=1
```

## ベンチマーク

```bash
cmake -B build_bench -DMENDO_BUILD_BENCHMARKS=ON
cmake --build build_bench --config Release --target mendo_bench -- //v:q //nologo
build_bench/bench/Release/mendo_bench.exe --benchmark_filter=<正規表現>
```

## 注意事項

- コメントは簡潔にまとめる。Whatコメントは禁止
- `third_party/` は外部コードなので編集しない
