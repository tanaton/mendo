# MaDView

**Mad** + **MD** + **D**irect2D + **View** — イカれた高速Markdownビュアー

Webブラウザ/WebViewを一切使わず、Direct2D/DirectWriteでMarkdownを直接描画するWindows向けネイティブアプリケーションです。

## 特徴

- **ブラウザレス描画** — Direct2D + DirectWrite による自前レンダリング。Chromium等のWebエンジンに依存しません
- **単一EXE** — CRTを静的リンクし、ランタイム不要でそのまま動作します
- **シンタックスハイライト** — C++, Python, JavaScript のコードブロックを色分け表示
- **スムーススクロール** — 60fpsアニメーションによる滑らかなスクロール
- **ファイル監視** — 編集中のファイルを自動検出してライブリロード
- **High DPI対応** — Per-Monitor DPI Awareness V2 に対応
- **ドラッグ&ドロップ** — `.md` ファイルをウィンドウにドロップするだけで表示
- **テキスト選択&コピー** — マウスで範囲選択し `Ctrl+C` でクリップボードにコピー

## 対応するMarkdown要素

| 要素 | 対応状況 |
|---|---|
| 見出し (H1-H6) | ✅ |
| 段落 | ✅ |
| 太字 / 斜体 / 取り消し線 | ✅ |
| インラインコード | ✅ |
| コードブロック (フェンス) | ✅ シンタックスハイライト付き |
| リンク (外部 / ページ内アンカー) | ✅ |
| 順序付きリスト / 箇条書き | ✅ ネスト対応 |
| タスクリスト | ✅ |
| 引用 (Blockquote) | ✅ |
| テーブル | ✅ アライメント対応 |
| 水平線 | ✅ |

## キーボードショートカット

| キー | 操作 |
|---|---|
| `Ctrl+O` | ファイルを開く |
| `Ctrl+C` | 選択テキストをコピー |
| `Ctrl+A` | 全選択 |
| `F5` | 再読み込み |
| `↑` `↓` | スクロール |
| `Page Up` `Page Down` | ページスクロール |
| `Home` `End` | 先頭/末尾へ移動 |
| `Esc` | 選択解除 |

## ビルド

### 必要なもの

- Windows 10 以降
- Visual Studio 2022 (MSVC, C++23対応)
- CMake 3.20+

### ビルド手順

```
cmake -B build
cmake --build build --config Release
```

生成される実行ファイル: `build/Release/MaDView.exe`

テストなしでビルドする場合:

```
cmake -B build -DMADVIEW_BUILD_TESTS=OFF
cmake --build build --config Release
```

### テスト実行

```
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

## 使い方

```
MaDView.exe [ファイルパス]
```

引数なしで起動した場合は、`Ctrl+O` またはドラッグ&ドロップでファイルを開けます。

## 技術スタック

| レイヤー | 技術 |
|---|---|
| 言語 | C++23 (MSVC) |
| GUI | Win32 API |
| 2D描画 | Direct2D (`ID2D1HwndRenderTarget`) |
| テキスト描画 | DirectWrite (`IDWriteTextLayout`) |
| Markdownパーサ | [md4c](https://github.com/mity/md4c) (SAX型コールバック) |
| テスト | Google Test v1.17.0 |
| ビルド | CMake 3.20+ |

## ライセンス

MIT
