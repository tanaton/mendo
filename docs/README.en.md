# mendo - Fast and simple Markdown viewer

A Markdown viewer that renders directly with Direct2D/DirectWrite. It is fast and uses little memory.

[日本語](../README.md) | **English** | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [한국어](README.ko.md)

![mendo screenshot](../example/image/mendo_light.png)

## Installation

Install via [winget](https://learn.microsoft.com/windows/package-manager/winget/):

```
winget install tanaton.mendo
```

### Recommended way to launch
Type `mendo` into the File Explorer address bar and press Enter.

## Features

- **Fast startup** — Window creation and file processing run in parallel for a quick launch.
- **Low memory usage** — Markdown is drawn directly with Direct2D + DirectWrite, without a browser engine.
- **Live reload** — Detects changes to the open file, updates the view automatically, and scrolls to the changed position.
- **Mermaid** — Renders `mermaid` code blocks as diagrams (offscreen rendering via WebView2 + Mermaid.js).
- **Offline** — Works without a network connection.
- **Syntax highlighting** — Colors code blocks in many programming languages, configuration file formats, and more.
- **Images** — Displays local PNG, JPEG, BMP, and other images.
- **LaTeX** — Renders `$$...$$` block equations and ` ```math ` code blocks with KaTeX. (For `$$...$$`, only paragraphs consisting solely of `$$...$$` are rendered; paragraphs mixing math with other text are shown as plain text.)
- **Navigation history** — Browser-style back/forward via mouse side buttons, mouse gestures, touchpad swipe, and more, with scroll position restored.
- **Drag & Drop** — Drop a Markdown file onto the window to open it.
- **In-file search** — Search within the file from the title bar search button or `Ctrl+F`.
- **Customizable view** — Show/hide each pane, switch between light and dark themes, zoom in 17 steps from 0.25x to 5.00x, and more.
- **Multilingual UI** — Shows the UI and help in Japanese, English, Simplified Chinese, Traditional Chinese, or Korean to match the Windows display language. You can also switch it from the settings button in the title bar.
- **Persistent settings** — Dark mode, zoom level, last opened file, and more are saved to `%LOCALAPPDATA%\mendo\`.

## Supported Markdown Elements

| Element | Status |
|---|---|
| Headings (H1-H6) | Supported |
| Paragraphs | Supported |
| Bold / Italic / Strikethrough | Supported |
| Inline code | Supported |
| Fenced code blocks | Supported (with syntax highlighting). Languages: C/C++, C#, Java, Kotlin, Swift, Python, JavaScript, TypeScript, Go, Rust, Zig, Haskell, Clojure, Emacs Lisp, Common Lisp, Scheme/Racket, Lua, Vim script, PHP, Ruby, SQL, HTML, XML, CSS, SCSS, JSON, YAML, TOML, INI, Bash, PowerShell, cmd (batch), Dockerfile, Makefile, diff |
| Images (PNG / JPEG / BMP) | Supported (async loading) |
| Mermaid diagrams | Supported (WebView2 rendering) |
| LaTeX math (`$$...$$` / ` ```math `) | Supported for block equations only. `$$...$$` is rendered when the paragraph consists solely of a `$$...$$` block; mixed paragraphs fall back to plain text. |
| GitHub Alerts | Supported (Note / Tip / Important / Warning / Caution) |
| Footnotes (`[^1]`) | Supported (listed at the end of the document, with links between references and definitions) |
| Links (external / page anchors) | Supported |
| Ordered / Unordered lists | Supported (nested) |
| Task lists | Supported |
| Blockquotes | Supported |
| Tables | Supported (with alignment) |
| Horizontal rules | Supported |

## Keyboard Shortcuts

| Key | Action |
|---|---|
| `Ctrl+O` | Open file |
| `Ctrl+C` | Copy selected text |
| `Ctrl+Shift+C` | Copy selected text as formatted (HTML) |
| `Ctrl+A` | Select all |
| `Ctrl+1` | Toggle file explorer |
| `Ctrl+2` | Toggle table of contents |
| `Ctrl+F` | Open search bar (closes it when pressed inside the search bar) |
| `F3` / `Ctrl+G` | Next match (`Enter` also works inside the search bar) |
| `Shift+F3` / `Ctrl+Shift+G` | Previous match (`Shift+Enter` also works inside the search bar) |
| `Ctrl++` / `Ctrl+-` | Zoom in / Zoom out |
| `Ctrl+0` | Reset zoom (100%) |
| `Ctrl+Mouse wheel` | Zoom in / Zoom out the area under the pointer (document, file pane, or TOC pane) |
| `Alt+Left` | Go back |
| `Alt+Right` | Go forward |
| `F1` | Show help |
| `F5` | Reload |
| `Up` `Down` | Scroll |
| `Page Up` `Page Down` | Page scroll |
| `Home` `End` | Jump to top / bottom |
| `Esc` | Close search bar / Deselect |

## Mouse Operations

| Operation | Action |
|---|---|
| Drag | Text selection |
| Double-click | Word selection |
| Right-click + horizontal drag | Mouse gesture (back / forward) |
| Touchpad horizontal swipe | Back / Forward |
| Side buttons | Back / Forward |
| Splitter drag | Resize pane width |
| Click link | Open external link in browser / Jump to anchor |
| Code block copy button | Copy code block as formatted text |
| Diagram (Mermaid / math) buttons | Save as PNG image / Copy to clipboard |

## Context Menu

| Item | Action |
|---|---|
| Left / Right buttons | Back / Forward |
| Open in Editor | Open current file in default editor |
| Copy | Copy selected text (when text is selected) |
| Copy as HTML | Copy selected text as HTML rich text. Preserves formatting when pasted into Word, Outlook, etc. (when text is selected) |
| Dark Mode | Toggle light/dark theme |
| File Pane | Toggle file explorer |
| TOC Pane | Toggle table of contents |

## Screenshots

![mendo with mermaid](../example/image/mendo_light_mermaid.png)

![mendo dark mode](../example/image/mendo_dark_test.png)

## Build

### Requirements

- Windows 10 or later
- Visual Studio 2026 (MSVC, C++23)
- CMake 4.2+ (required for the Visual Studio 2026 generator)

### Build Steps

```
cmake -B build
cmake --build build --config Release
```

Output: `build/Release/mendo.exe`

Build without tests:

```
cmake -B build -DMENDO_BUILD_TESTS=OFF
cmake --build build --config Release
```

### Run Tests

```
cmake --build build --config Release
build/tests/Release/mendo_tests.exe
```

### Build with Tracy Profiler (developers)

Embeds [Tracy](https://github.com/wolfpld/tracy) v0.14.1 for frame timing, zone tracing, and counter time-series plots. Tracy is built in `TRACY_ON_DEMAND` mode, so instrumentation is a no-op until a Tracy GUI client connects.

```
cmake -B build_tracy -DMENDO_USE_TRACY=ON
cmake --build build_tracy --config Release
```

Launch the Tracy GUI (download from [Releases](https://github.com/wolfpld/tracy/releases)) and then run `build_tracy/Release/mendo.exe` to connect. Tracy is only linked and instrumented when `MENDO_USE_TRACY=ON`; the default build (OFF) is completely unaffected.

## Usage

```
mendo.exe [filepath | folderpath]
```

| Argument | Document shown | File pane folder |
|---|---|---|
| none | last opened file | current directory |
| file | the given file | the file's folder |
| folder | last opened file | the given folder |

When the current directory is a system folder or mendo.exe's folder (e.g. launched from the Start menu), the last file's folder is shown instead. The button at the top of the file pane jumps to the folder of the file being viewed.

If there is no last opened file, open a file with `Ctrl+O` or drag & drop.

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++23 (MSVC) |
| GUI | Win32 API |
| 2D rendering | Direct2D (`ID2D1DeviceContext`) |
| Text rendering | DirectWrite (`IDWriteTextLayout`) |
| Diagrams | WebView2 + [Mermaid.js](https://mermaid.js.org/) |
| Math | [KaTeX](https://katex.org/) (via Mermaid) |
| Markdown parser | [md4c](https://github.com/mity/md4c) (SAX-style callbacks) |
| Tests | Google Test v1.18.0 |
| Build | CMake 4.2+ |

## License

[MIT](../LICENSE)

See [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md) for third-party library licenses.
