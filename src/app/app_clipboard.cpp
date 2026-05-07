#include "app.h"
#include "document_utils.h"
#include "mermaid_util.h"
#include "mermaid_file_cache.h"
#include "win_handle.h"
#include "file_io.h"
#include "i18n.h"
#include "string_convert.h"
#include <commdlg.h>

void App::OnLButtonDblClk(int px, int py)
{
    if (!IsRenderReady()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    // CS_DBLCLKS により連続クリックの2回目は WM_LBUTTONDBLCLK になるため、
    // タイトルバーボタンのクリックを先に処理する。
    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
    }
    const auto zone = PaneAtPoint(dip.x);
    if (zone != PaneZone::MdPane) {
        return;
    }
    // 検索バーのボタンも連打として扱う (タイトルバーボタンと同じ理由)。
    const auto& layout = GetPaneLayout();
    if (HandleSearchBarClick(dip.x, dip.y, layout, true)) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index < 0) {
        return;
    }
    const auto& node = state_.document.doc.GetNodes()[hit.node_index];
    const std::string_view text = node.LinearizedText();
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) {
        return;
    }

    state_.view.viewport.SetAnchor(hit.node_index, wb.start);
    state_.view.viewport.SetSelection(TextSelection::MakeOrdered(hit.node_index, wb.start, hit.node_index, wb.end));
    InvalidateMdPane(layout.md_rect);
}

void App::SetClipboardText(std::wstring_view text) const
{
    WriteClipboardText(hwnd_, text);
}

void App::CopySelectionToClipboard() const
{
    if (!state_.view.viewport.GetSelection().active) {
        return;
    }
    const std::pmr::string result = ExtractSelectedText(state_.document.doc.GetNodes(), state_.view.viewport.GetSelection());
    // CF_UNICODETEXT は wstring 必須なので UTF-8 → wstring 変換。
    std::pmr::wstring wide;
    string_convert::Utf8ToWide(result, wide);
    SetClipboardText(wide);
}

void App::CopyCodeBlockToClipboard(int node_index) const
{
    const auto& nodes = state_.document.doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    std::pmr::wstring wide;
    string_convert::Utf8ToWide(nodes[node_index].GetText(), wide);
    SetClipboardText(wide);
}

void App::SaveDiagramAsPng(int node_index)
{
    const auto& nodes = state_.document.doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    const auto& node = nodes[node_index];
    if (node.type != NodeType::CodeBlock || !IsDiagramLanguage(node.code_language)) {
        return;
    }

    const float md_width = renderer_.GetTheme().ContentWidth(GetMarkdownPaneWidth());
    const bool dark = renderer_.GetTheme().IsDark();
    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob png;
    if (!file_cache_.Lookup(key, entry, png) || png.size == 0) {
        return;
    }

    wchar_t filename[MAX_PATH] = L"diagram.png";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"png";

    if (!GetSaveFileNameW(&ofn)) {
        return;
    }

    if (WriteAllBytes(filename, png.data.get(), png.size)) {
        ShowToast(i18n::S().toast_image_saved);
    }
    else {
        DeleteFileW(filename);
    }
}

void App::CopyDiagramAsSvg(int node_index)
{
    const auto& nodes = state_.document.doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    const auto& node = nodes[node_index];
    if (node.type != NodeType::CodeBlock || !IsSvgExportable(node.code_language)) {
        return;
    }
    if (svg_copy_in_flight_) {
        return;
    }

    const float md_width = renderer_.GetTheme().ContentWidth(GetMarkdownPaneWidth());
    const bool dark = renderer_.GetTheme().IsDark();
    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    if (const auto* hit = svg_cache_.Find(key)) {
        const bool ok = WriteClipboardSvg(hwnd_, *hit);
        ShowToast(ok ? i18n::S().toast_svg_copied : i18n::S().toast_svg_copy_failed);
        return;
    }

    ShowToast(i18n::S().toast_svg_copying);
    svg_copy_in_flight_ = true;

    // RequestSvg は WebView2 経路のため wstring。UTF-8 → wide に変換して渡す。
    std::pmr::wstring code_wide;
    string_convert::Utf8ToWide(node.GetText(), code_wide);
    const std::wstring_view code_view = code_wide;
    mermaid_renderer_.RequestSvg(code_view, md_width, dark, [this, key](std::pmr::wstring svg, bool cancelled) {
        svg_copy_in_flight_ = false;
        if (cancelled) {
            // テーマ変更/幅変更/シャットダウン等によるキャンセル。トーストは出さない。
            return;
        }
        if (svg.empty()) {
            ShowToast(i18n::S().toast_svg_copy_failed);
            return;
        }
        const bool ok = WriteClipboardSvg(hwnd_, svg);
        if (ok) {
            svg_cache_.Insert(key, std::move(svg));
        }
        ShowToast(ok ? i18n::S().toast_svg_copied : i18n::S().toast_svg_copy_failed);
    });
}
