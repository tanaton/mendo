# mendo - 高速＆省メモリMarkdownビュアー

## プロジェクト概要

Webブラウザを使わず、Direct2D/DirectWriteによる自前レンダリングでWindows向け高速Markdownビュアーを実現するネイティブアプリケーション。

## 技術スタック

- **言語**: C++23 (MSVC)
- **GUI**: Win32 API (`WNDCLASSEXW` + メッセージループ)
- **描画**: Direct2D (`ID2D1HwndRenderTarget`)
- **テキスト**: DirectWrite (`IDWriteTextLayout`)
- **Markdownパーサ**: md4c (SAX型コールバック、`third_party/md4c/`)
- **テスト**: Google Test v1.17.0 (FetchContentで取得)
- **ビルド**: CMake 3.20+

## ビルド方法

```
cmake -B build
cmake --build build --config Release -- //v:q //nologo
```

テストなしでビルドする場合:
```
cmake -B build -DMENDO_BUILD_TESTS=OFF
cmake --build build --config Release -- //v:q //nologo
```

## テスト実行

```
cmake --build build --config Release -- //v:q //nologo
ctest --test-dir build --output-on-failure -C Release -j0 2>&1 | grep -vE "^\s*(Start|[0-9]+/[0-9]+.*Passed)"
```

## 注意事項

- 思考過程も日本語で出力してね
- `third_party/` は外部コードなので編集しない
- マニフェストは `res/mendo.rc` 経由で埋め込み。`#pragma comment(linker, "/manifestdependency:...")` は使わない
