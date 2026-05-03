# mendo - 高速で簡単なMarkdownビュアー

Direct2D/DirectWriteを使用して直接描画を行うMarkdownビュアー。高速な動作と低メモリ使用量を実現しています。

**[English](README.en.md)**

![mendoのようす](./example/image/mendo_light.png)

## インストール

[winget](https://learn.microsoft.com/windows/package-manager/winget/) でインストールできます。

```
winget install tanaton.mendo
```

## 特徴

- **ゼロHTMLレンダリング** — MarkdownをHTMLに変換せず、Direct2D + DirectWrite で直接描画
- **Mermaidダイアグラム** — `mermaid` コードブロックを図として表示（WebView2によるオフスクリーンレンダリング）
- **LaTeX数式** — `$$...$$` のブロック数式を KaTeX で描画（`$$...$$` のみからなる段落が対象。テキストと混在する段落は通常テキスト表示）
- **完全オフライン動作** — Mermaid.js 等のリソースも実行ファイルに同梱、ネットワーク接続不要
- **ファイル監視** — 編集中のファイルを自動検出してライブリロード
- **シンタックスハイライト** — C++, Python, JavaScript, Go, Rust, pwsh, bash, cmd のコードブロックを色分け表示
- **ナビゲーション履歴** — `Alt+←` / `Alt+→` / マウスサイドボタン / マウスジェスチャー / タッチパッドスワイプでブラウザスタイルの戻る/進む。スクロール位置も復元
- **画像表示** — PNG, JPEG, BMP等の画像をMarkdown内に表示
- **ズーム** — `Ctrl++` / `Ctrl+-` / `Ctrl+マウスホイール` で0.25x〜5.00xの17段階ズーム
- **ドラッグ&ドロップ** — `.md` ファイルをウィンドウにドロップするだけで表示
- **3ペインレイアウト** — ファイルエクスプローラー / 目次 / Markdownコンテンツの3ペイン構成
- **ダークモード** — ライト/ダークテーマを切り替え
- **設定の永続化** — ダークモード・ズームレベル・最後に開いたファイル等を `%LOCALAPPDATA%\mendo\` に保存

## 対応するMarkdown要素

| 要素 | 対応状況 |
|---|---|
| 見出し (H1-H6) | ✅ |
| 段落 | ✅ |
| 太字 / 斜体 / 取り消し線 | ✅ |
| インラインコード | ✅ |
| コードブロック (フェンス) | ✅ シンタックスハイライト付き |
| 画像 (PNG / JPEG / BMP) | ✅ 非同期読み込み |
| Mermaidダイアグラム | ✅ WebView2でレンダリング |
| LaTeX数式 (`$$...$$`) | ✅ ブロック数式のみ。段落が `$$...$$` のみで構成される場合に描画、他のテキストと混在する段落はテキスト表示 |
| GitHub Alerts | ✅ Note / Tip / Important / Warning / Caution |
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
| `Ctrl+Shift+C` | 選択テキストを書式付き（HTML）でコピー |
| `Ctrl+A` | 全選択 |
| `Ctrl+1` | ファイルエクスプローラーの表示/非表示 |
| `Ctrl+2` | 目次の表示/非表示 |
| `Ctrl++` / `Ctrl+-` | ズームイン / ズームアウト |
| `Ctrl+0` | ズームをリセット (100%) |
| `Ctrl+マウスホイール` | ズームイン / ズームアウト |
| `Alt+←` | 戻る |
| `Alt+→` | 進む |
| `F1` | 操作ガイドを表示 |
| `F5` | 再読み込み |
| `↑` `↓` | スクロール |
| `Page Up` `Page Down` | ページスクロール |
| `Home` `End` | 先頭/末尾へ移動 |
| `Esc` | 選択解除 |

## マウス操作

| 操作 | 動作 |
|---|---|
| ドラッグ | テキスト範囲選択 |
| ダブルクリック | 単語選択 |
| 右クリック+左右ドラッグ | マウスジェスチャーで戻る/進む |
| タッチパッド水平スワイプ | 戻る / 進む |
| サイドボタン | 戻る / 進む |
| スプリッタードラッグ | ペイン幅の調整 |
| リンクをクリック | 外部リンクをブラウザで開く / ページ内アンカーにジャンプ |
| コードブロックのコピーボタン | コードブロックの内容をクリップボードにコピー |

## 右クリックメニュー

| 項目 | 操作 |
|---|---|
| ← → ボタン | 戻る / 進む |
| エディタで開く | 現在のファイルを既定のエディタで開く |
| コピー | 選択テキストをコピー（テキスト選択時のみ） |
| 書式付きコピー | 選択テキストをHTMLリッチテキストでコピー。WordやOutlook等に貼ると書式を保持（テキスト選択時のみ） |
| ダークモード | ライト/ダークテーマの切り替え |

## スクリーンショット

![mendoでmermaid](./example/image/mendo_light_mermaid.png)

![mendoのダークモード](./example/image/mendo_dark_test.png)

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

生成される実行ファイル: `build/Release/mendo.exe`

テストなしでビルドする場合:

```
cmake -B build -DMENDO_BUILD_TESTS=OFF
cmake --build build --config Release
```

### テスト実行

```
cmake --build build --config Release
build/tests/Release/mendo_tests.exe
```

### Tracyプロファイラ有効ビルド（開発者向け）

[Tracy](https://github.com/wolfpld/tracy) v0.13.1 を組み込み、フレーム時間・ゾーン計測・カウンタ時系列プロットを取得できます。Tracy GUI 接続前は計測コードが no-op になる `TRACY_ON_DEMAND` モードで組み込まれます。

```
cmake -B build_tracy -DMENDO_USE_TRACY=ON
cmake --build build_tracy --config Release
```

Tracy GUI（[Releases](https://github.com/wolfpld/tracy/releases) からダウンロード）を起動した状態で `build_tracy/Release/mendo.exe` を実行すると接続できます。`MENDO_USE_TRACY=ON` のときのみ Tracy がリンク・計装され、デフォルト（OFF）のビルド成果物には一切影響しません。

## 使い方

```
mendo.exe [ファイルパス]
```

引数なしで起動した場合は、`Ctrl+O` またはドラッグ&ドロップでファイルを開けます。

## 技術スタック

| レイヤー | 技術 |
|---|---|
| 言語 | C++23 (MSVC) |
| GUI | Win32 API |
| 2D描画 | Direct2D (`ID2D1HwndRenderTarget`) |
| テキスト描画 | DirectWrite (`IDWriteTextLayout`) |
| ダイアグラム | WebView2 + [Mermaid.js](https://mermaid.js.org/) |
| Markdownパーサ | [md4c](https://github.com/mity/md4c) (SAX型コールバック) |
| テスト | Google Test v1.17.0 |
| ビルド | CMake 3.20+ |

## ライセンス

[MIT](LICENSE)

サードパーティライブラリのライセンスについては [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) を参照してください。
