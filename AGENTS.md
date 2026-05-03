# mendo - 高速＆省メモリMarkdownビュアー

## プロジェクト概要

Direct2D/DirectWriteを使用して直接描画を行うMarkdownビュアー。

## 技術スタック

- **言語**: C++23 (MSVC)
- **GUI**: Win32 API (`WNDCLASSEXW` + メッセージループ)
- **描画**: Direct2D (`ID2D1Factory1` + `ID2D1DeviceContext`)
- **テキスト**: DirectWrite (`IDWriteTextLayout`)
- **Markdownパーサ**: md4c (SAX型コールバック、`third_party/md4c/`)
- **テスト**: Google Test v1.17.0
- **ビルド**: CMake 3.20+

## ビルド方法

```bash
cmake -B build
cmake --build build --config Release -- //v:q //nologo
```

## テスト実行

```bash
cmake --build build --config Release -- //v:q //nologo
build/tests/Release/mendo_tests.exe --gtest_brief=1
```

## 注意事項

- コメントは簡潔にまとめる。Whatコメントは禁止
- `third_party/` は外部コードなので編集しない
