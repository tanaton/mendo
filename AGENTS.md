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

- 時間を測るコードは gtest の `DISABLED_` テストではなく `bench/benchmarks.cpp` に書く
- 比べるのは Time 列だけ。CPU 列は Windows では粒度が粗く、`manual_time` のベンチでは準備も含む
- 前後の比較は `--benchmark_repetitions=10 --benchmark_report_aggregates_only=true` の median で見る。同じ exe を続けて 2 回走らせても ±5% ほどずれるので、5% に満たない差は前後の exe を交互に何度か走らせて確かめる

## 注意事項

- コメントは簡潔にまとめる。Whatコメントは禁止
- `third_party/` は外部コードなので編集しない
