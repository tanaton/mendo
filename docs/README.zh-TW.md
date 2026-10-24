# mendo - 快速、簡單的 Markdown 檢視器

使用 Direct2D/DirectWrite 直接繪製的 Markdown 檢視器。執行速度快，佔用記憶體少。

[日本語](../README.md) | [English](README.en.md) | [简体中文](README.zh-CN.md) | **繁體中文** | [한국어](README.ko.md)

![mendo 螢幕擷取畫面](../example/image/mendo_light.png)

## 安裝

可以透過 [winget](https://learn.microsoft.com/windows/package-manager/winget/) 安裝：

```
winget install tanaton.mendo
```

### 建議的啟動方式
在檔案總管的網址列中輸入 `mendo` 並按 Enter 鍵。

## 特色

- **快速啟動** — 視窗建立與檔案處理平行進行，啟動迅速。
- **低記憶體用量** — 不使用瀏覽器引擎，透過 Direct2D + DirectWrite 直接繪製 Markdown。
- **即時重新載入** — 偵測到開啟的檔案有變更時自動更新顯示，並捲動到變更的位置。
- **Mermaid** — 將 `mermaid` 程式碼區塊顯示為圖表（透過 WebView2 + Mermaid.js 進行離屏繪製）。
- **離線運作** — 不需要網路連線即可使用。
- **語法醒目提示** — 為多種程式語言、設定檔格式等的程式碼區塊上色。
- **圖片顯示** — 顯示本機的 PNG、JPEG、BMP 等圖片。
- **LaTeX** — 使用 KaTeX 繪製 `$$...$$` 區塊公式和 ` ```math ` 程式碼區塊。（`$$...$$` 僅在段落只由 `$$...$$` 構成時繪製，與其他文字混排的段落以一般文字顯示。）
- **瀏覽記錄** — 支援滑鼠側鍵、滑鼠手勢、觸控板滑動等瀏覽器式的上一頁 / 下一頁，並還原捲動位置。
- **拖放** — 將 Markdown 檔案拖放到視窗中即可開啟。
- **檔案內搜尋** — 透過標題列的搜尋按鈕或 `Ctrl+F` 在檔案內搜尋。
- **外觀自訂** — 可切換各窗格的顯示 / 隱藏、切換淺色 / 深色佈景主題、在 0.25x 到 5.00x 之間以 17 段縮放等。
- **多語言介面** — 依照 Windows 的顯示語言，以日文、英文、簡體中文、繁體中文或韓文顯示介面和說明。也可以透過標題列的設定按鈕切換。
- **設定保存** — 深色模式、縮放比例、上次開啟的檔案等會儲存在 `%LOCALAPPDATA%\mendo\` 中。

## 支援的 Markdown 元素

| 元素 | 支援情況 |
|---|---|
| 標題 (H1-H6) | ✅ |
| 段落 | ✅ |
| 粗體 / 斜體 / 刪除線 | ✅ |
| 行內程式碼 | ✅ |
| 程式碼區塊（圍欄） | ✅ 附語法醒目提示。支援的語言：C/C++, C#, Java, Kotlin, Swift, Python, JavaScript, TypeScript, Go, Rust, Zig, Haskell, Clojure, Emacs Lisp, Common Lisp, Scheme/Racket, Lua, Vim script, PHP, Ruby, SQL, HTML, XML, CSS, SCSS, JSON, YAML, TOML, INI, Bash, PowerShell, cmd（批次檔）, Dockerfile, Makefile, diff |
| 圖片 (PNG / JPEG / BMP) | ✅ 非同步載入 |
| Mermaid 圖表 | ✅ 使用 WebView2 繪製 |
| LaTeX 公式 (`$$...$$` / ` ```math `) | ✅ 僅支援區塊公式。`$$...$$` 在段落只由 `$$...$$` 構成時繪製，與其他文字混排的段落以文字顯示 |
| GitHub Alerts | ✅ Note / Tip / Important / Warning / Caution |
| 註腳 (`[^1]`) | ✅ 依編號顯示在文件結尾，參照與定義互相連結 |
| 連結（外部 / 頁內錨點） | ✅ |
| 編號清單 / 項目符號清單 | ✅ 支援巢狀 |
| 工作清單 | ✅ |
| 引言 (Blockquote) | ✅ |
| 表格 | ✅ 支援對齊 |
| 水平線 | ✅ |

## 鍵盤快速鍵

| 按鍵 | 操作 |
|---|---|
| `Ctrl+O` | 開啟檔案 |
| `Ctrl+C` | 複製所選文字 |
| `Ctrl+Shift+C` | 將所選文字複製為帶格式的 HTML |
| `Ctrl+A` | 全選 |
| `Ctrl+1` | 顯示 / 隱藏檔案窗格 |
| `Ctrl+2` | 顯示 / 隱藏目錄窗格 |
| `Ctrl+F` | 開啟搜尋列（在搜尋列內按下時關閉） |
| `F3` / `Ctrl+G` | 下一個相符項目（在搜尋列內也可按 `Enter`） |
| `Shift+F3` / `Ctrl+Shift+G` | 上一個相符項目（在搜尋列內也可按 `Shift+Enter`） |
| `Ctrl++` / `Ctrl+-` | 放大 / 縮小 |
| `Ctrl+0` | 重設縮放 (100%) |
| `Ctrl+滑鼠滾輪` | 放大 / 縮小指標下方的區域（內文、檔案窗格或目錄窗格） |
| `Alt+←` | 上一頁 |
| `Alt+→` | 下一頁 |
| `F1` | 顯示說明 |
| `F5` | 重新載入 |
| `↑` `↓` | 捲動 |
| `Page Up` `Page Down` | 逐頁捲動 |
| `Home` `End` | 跳到開頭 / 結尾 |
| `Esc` | 關閉搜尋列 / 取消選取 |

## 滑鼠操作

| 操作 | 動作 |
|---|---|
| 拖曳 | 選取文字 |
| 按兩下 | 選取單字 |
| 按住右鍵左右拖曳 | 滑鼠手勢（上一頁 / 下一頁） |
| 觸控板水平滑動 | 上一頁 / 下一頁 |
| 側鍵 | 上一頁 / 下一頁 |
| 拖曳分隔線 | 調整窗格寬度 |
| 按一下連結 | 在瀏覽器中開啟外部連結 / 跳到頁內錨點 |
| 程式碼區塊的複製按鈕 | 將程式碼區塊內容複製為帶格式的文字 |
| 圖表（Mermaid / 公式）的按鈕 | 儲存為 PNG 圖片 / 複製到剪貼簿 |

## 右鍵選單

| 項目 | 操作 |
|---|---|
| ← → 按鈕 | 上一頁 / 下一頁 |
| 在編輯器中開啟 | 以預設編輯器開啟目前的檔案 |
| 複製 | 複製所選文字（僅在選取文字時） |
| 複製為 HTML | 將所選文字複製為 HTML 富文字。貼到 Word、Outlook 等時保留格式（僅在選取文字時） |
| 深色模式 | 切換淺色 / 深色佈景主題 |
| 檔案窗格 | 顯示 / 隱藏檔案窗格 |
| 目錄窗格 | 顯示 / 隱藏目錄窗格 |

## 螢幕擷取畫面

![mendo 顯示 Mermaid](../example/image/mendo_light_mermaid.png)

![mendo 深色模式](../example/image/mendo_dark_test.png)

## 建置

### 需求

- Windows 10 或更新版本
- Visual Studio 2026（MSVC，支援 C++23）
- CMake 4.2+（支援 Visual Studio 2026 產生器的版本）

### 建置步驟

```
cmake -B build
cmake --build build --config Release
```

產生的執行檔：`build/Release/mendo.exe`

不建置測試時：

```
cmake -B build -DMENDO_BUILD_TESTS=OFF
cmake --build build --config Release
```

### 執行測試

```
cmake --build build --config Release
build/tests/Release/mendo_tests.exe
```

### 基準測試（開發者適用）

使用 [Google Benchmark](https://github.com/google/benchmark) v1.10.0 量測剖析與版面配置的耗時。預設（OFF）的建置不包含它。

```
cmake -B build_bench -DMENDO_BUILD_BENCHMARKS=ON
cmake --build build_bench --config Release --target mendo_bench
build_bench/bench/Release/mendo_bench.exe --benchmark_filter=FromMarkdown
```

修改前後請用多次重複執行的中位數（median）進行比較。

```
build_bench/bench/Release/mendo_bench.exe --benchmark_filter=FromMarkdown --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
```

### 啟用 Tracy 效能分析工具的建置（開發者適用）

內嵌 [Tracy](https://github.com/wolfpld/tracy) v0.14.1，可取得影格時間、區段計時和計數器時間序列圖。Tracy 以 `TRACY_ON_DEMAND` 模式內嵌，在 Tracy GUI 連線之前，量測程式碼不執行任何動作。

```
cmake -B build_tracy -DMENDO_USE_TRACY=ON
cmake --build build_tracy --config Release
```

啟動 Tracy GUI（從 [Releases](https://github.com/wolfpld/tracy/releases) 下載）後執行 `build_tracy/Release/mendo.exe` 即可連線。只有在 `MENDO_USE_TRACY=ON` 時才會連結並植入 Tracy，預設（OFF）的建置成品完全不受影響。

## 使用方式

```
mendo.exe [檔案路徑 | 資料夾路徑]
```

| 引數 | 顯示的檔案 | 檔案窗格的資料夾 |
|---|---|---|
| 無 | 上次開啟的檔案 | 目前的資料夾 |
| 檔案 | 指定的檔案 | 該檔案所在的資料夾 |
| 資料夾 | 上次開啟的檔案 | 指定的資料夾 |

目前的資料夾是系統資料夾或 mendo.exe 所在的資料夾時（例如從開始功能表啟動），會顯示上次開啟的檔案所在的資料夾。使用檔案窗格上方的按鈕可以前往正在顯示的檔案所在的資料夾。

如果沒有上次開啟的檔案，可以按 `Ctrl+O` 或拖放來開啟檔案。

## 技術堆疊

| 層 | 技術 |
|---|---|
| 語言 | C++23 (MSVC) |
| GUI | Win32 API |
| 2D 繪製 | Direct2D (`ID2D1DeviceContext`) |
| 文字繪製 | DirectWrite (`IDWriteTextLayout`) |
| 圖表 | WebView2 + [Mermaid.js](https://mermaid.js.org/) |
| 公式 | [KaTeX](https://katex.org/)（透過 Mermaid） |
| Markdown 剖析器 | [md4c](https://github.com/mity/md4c)（SAX 式回呼） |
| 測試 | Google Test v1.18.0 |
| 基準測試 | Google Benchmark v1.10.0 |
| 建置 | CMake 4.2+ |

## 授權

[MIT](../LICENSE)

第三方程式庫的授權請參閱 [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md)。
