#include "app.h"
#include "document_utils.h"
#include "pane_layout.h"
#include "mermaid_util.h"
#include "mermaid_file_cache.h"
#include "i18n.h"
#include <commdlg.h>

void App::OnLButtonDblClk(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    const auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index < 0) {
        return;
    }
    const auto& text = doc_.GetNodes()[hit.node_index].GetText();
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) {
        return;
    }

    viewport_.SetAnchor(hit.node_index, wb.start);
    viewport_.SetSelection(TextSelection::MakeOrdered(hit.node_index, wb.start, hit.node_index, wb.end));
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

// ============================================================
// 選択 / クリップボード
// ============================================================

void App::ClearSelection()
{
    viewport_.ClearSelection();
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

void App::SelectAll()
{
    viewport_.SelectAll(doc_.GetNodes());
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

void App::SetClipboardText(std::wstring_view text) const
{
    if (text.empty()) {
        return;
    }
    if (!OpenClipboard(hwnd_)) {
        return;
    }
    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* ptr = GlobalLock(hMem);
        if (ptr) {
            memcpy(ptr, text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(ptr)[text.size()] = L'\0';
            GlobalUnlock(hMem);
            if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                GlobalFree(hMem);
            }
        }
        else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

void App::CopySelectionToClipboard() const
{
    if (!viewport_.GetSelection().active) {
        return;
    }
    const std::pmr::wstring result = ExtractSelectedText(doc_.GetNodes(), viewport_.GetSelection());
    SetClipboardText(result);
}

void App::CopyCodeBlockToClipboard(int node_index) const
{
    const auto& nodes = doc_.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    SetClipboardText(nodes[node_index].GetText());
}

void App::SaveDiagramAsPng(int node_index)
{
    const auto& nodes = doc_.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    const auto& node = nodes[node_index];
    if (node.type != NodeType::CodeBlock || node.code_language != SyntaxLanguage::Mermaid) {
        return;
    }

    // ファイルキャッシュからPNGデータを取得
    const float md_width = renderer_.GetTheme().ContentWidth(GetMarkdownPaneWidth());
    const bool dark = renderer_.GetTheme().IsDark();
    const uint64_t key = mermaid_util::HashCode(node.text_utf8, md_width, dark);

    MermaidFileCache::CacheEntry entry;
    std::vector<uint8_t> png_data;
    if (!file_cache_.Lookup(key, entry, png_data) || png_data.empty()) {
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
    HANDLE hFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(png_data.size());
    const BOOL ok = WriteFile(hFile, png_data.data(), size, &written, nullptr);
    CloseHandle(hFile);

    if (ok && written == size) {
        ShowToast(i18n::S().toast_image_saved);
    }
}
