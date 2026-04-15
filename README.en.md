# mendo - Simple Markdown Viewer for Windows

A lightweight, single-EXE Markdown viewer — just drop a file and start reading.

**[Japanese / 日本語](README.md)**

![mendo screenshot](./example/image/mendo_light.png)

## Features

- **Browser-less rendering** — Direct2D + DirectWrite native rendering
- **Single EXE** — Statically linked CRT, no runtime dependencies
- **3-pane layout** — File explorer / Table of contents / Markdown content
- **File explorer** — Browse files and folders in the same directory
- **Table of contents (TOC)** — Auto-extracted headings with click-to-jump and hover highlight
- **Mermaid diagrams** — Render `mermaid` code blocks as diagrams (via offscreen WebView2)
- **Dark mode** — Toggle between light and dark themes
- **Syntax highlighting** — C++, Python, JavaScript, Go, Rust, PowerShell, Bash, cmd
- **Navigation history** — `Alt+Left/Right`, mouse side buttons, mouse gestures, touchpad swipe with scroll position restoration
- **Image display** — PNG, JPEG, BMP and more
- **Zoom** — `Ctrl++` / `Ctrl+-` / `Ctrl+Mouse wheel` (0.25x to 5.00x, 17 levels)
- **File watching** — Auto-reload on external file changes
- **Drag & Drop** — Drop `.md` files onto the window to open
- **Persistent settings** — Dark mode, zoom level, last opened file saved to `%LOCALAPPDATA%\mendo\`

## Supported Markdown Elements

| Element | Status |
|---|---|
| Headings (H1-H6) | Supported |
| Paragraphs | Supported |
| Bold / Italic / Strikethrough | Supported |
| Inline code | Supported |
| Fenced code blocks | Supported (with syntax highlighting) |
| Images (PNG / JPEG / BMP) | Supported (async loading) |
| Mermaid diagrams | Supported (WebView2 rendering) |
| GitHub Alerts | Supported (Note / Tip / Important / Warning / Caution) |
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
| `Ctrl+A` | Select all |
| `Ctrl+F` | Find |
| `Ctrl+1` | Toggle file explorer |
| `Ctrl+2` | Toggle table of contents |
| `Ctrl++` / `Ctrl+-` | Zoom in / Zoom out |
| `Ctrl+0` | Reset zoom (100%) |
| `Ctrl+Mouse wheel` | Zoom in / Zoom out |
| `Alt+Left` | Go back |
| `Alt+Right` | Go forward |
| `F1` | Show help |
| `F5` | Reload |
| `Up` `Down` | Scroll |
| `Page Up` `Page Down` | Page scroll |
| `Home` `End` | Jump to top / bottom |
| `Esc` | Deselect |

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
| Code block copy button | Copy code block to clipboard |

## Context Menu

| Item | Action |
|---|---|
| Left / Right buttons | Back / Forward |
| Open in editor | Open current file in default editor |
| Copy | Copy selected text (when text is selected) |
| Dark mode | Toggle light/dark theme |

## Screenshots

![mendo with mermaid](./example/image/mendo_light_mermaid.png)

![mendo dark mode](./example/image/mendo_dark_test.png)

## Build

### Requirements

- Windows 10 or later
- Visual Studio 2022 (MSVC, C++23)
- CMake 3.20+

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

## Usage

```
mendo.exe [filepath]
```

If launched without arguments, open a file with `Ctrl+O` or drag & drop.

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++23 (MSVC) |
| GUI | Win32 API |
| 2D rendering | Direct2D (`ID2D1HwndRenderTarget`) |
| Text rendering | DirectWrite (`IDWriteTextLayout`) |
| Diagrams | WebView2 + [Mermaid.js](https://mermaid.js.org/) |
| Markdown parser | [md4c](https://github.com/mity/md4c) (SAX-style callbacks) |
| Tests | Google Test v1.17.0 |
| Build | CMake 3.20+ |

## License

[MIT](LICENSE)

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for third-party library licenses.
