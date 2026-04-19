#include "app.h"
#include "document_utils.h"
#include "mermaid_util.h"
#include "mermaid_file_cache.h"
#include "win_handle.h"
#include "file_io.h"
#include "i18n.h"
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
    const auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index < 0) {
        return;
    }
    const auto& text = state_.document.doc.GetNodes()[hit.node_index].GetText();
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) {
        return;
    }

    state_.view.viewport.SetAnchor(hit.node_index, wb.start);
    state_.view.viewport.SetSelection(TextSelection::MakeOrdered(hit.node_index, wb.start, hit.node_index, wb.end));
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

// ============================================================
// 選択 / クリップボード
// ============================================================

void App::SetClipboardText(std::wstring_view text) const
{
    WriteClipboardText(hwnd_, text);
}

void App::CopySelectionToClipboard() const
{
    if (!state_.view.viewport.GetSelection().active) {
        return;
    }
    const std::pmr::wstring result = ExtractSelectedText(state_.document.doc.GetNodes(), state_.view.viewport.GetSelection());
    SetClipboardText(result);
}

void App::CopyCodeBlockToClipboard(int node_index) const
{
    const auto& nodes = state_.document.doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    SetClipboardText(nodes[node_index].GetText());
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

    // ファイルキャッシュからPNGデータを取得
    const float md_width = renderer_.GetTheme().ContentWidth(GetMarkdownPaneWidth());
    const bool dark = renderer_.GetTheme().IsDark();
    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob png;
    if (!file_cache_.Lookup(key, entry, png) || png.size == 0) {
        return;
    }

    // 保存先をユーザーに選択させる
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

    // PNGデータをファイルに書き出す
    if (WriteAllBytes(filename, png.data.get(), png.size)) {
        ShowToast(i18n::S().toast_image_saved);
    } else {
        DeleteFileW(filename);
    }
}
