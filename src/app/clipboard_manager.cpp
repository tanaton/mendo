#include "clipboard_manager.h"
#include "clipboard_util.h"
#include "document.h"
#include "file_dialog_service.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_file_cache.h"
#include "mermaid_util.h"
#include "string_convert.h"

namespace {

// node_index がノード数の範囲内なら該当ノードを、範囲外なら nullptr を返す。
const Node* NodeAt(const Document& doc, int node_index) noexcept
{
    const auto& nodes = doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return nullptr;
    }
    return &nodes[node_index];
}

} // namespace

void ClipboardManager::Init(HWND hwnd, MermaidFileCache* file_cache, IMermaidRenderer* mermaid_renderer,
                            ToastCallback show_toast) noexcept
{
    hwnd_ = hwnd;
    file_cache_ = file_cache;
    mermaid_renderer_ = mermaid_renderer;
    show_toast_ = std::move(show_toast);
}

void ClipboardManager::CopyCodeBlock(const Document& doc, int node_index) const
{
    const Node* node = NodeAt(doc, node_index);
    if (!node) {
        return;
    }
    std::pmr::wstring wide;
    string_convert::Utf8ToWide(node->GetText(), wide);
    WriteClipboardText(hwnd_, wide);
}

void ClipboardManager::SaveDiagramAsPng(const Document& doc, int node_index, float md_width, bool dark)
{
    const Node* node_ptr = NodeAt(doc, node_index);
    if (!node_ptr) {
        return;
    }
    const auto& node = *node_ptr;
    if (node.type != NodeType::CodeBlock || !IsDiagramLanguage(node.code_language)) {
        return;
    }
    if (!file_cache_) {
        return;
    }

    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob png;
    if (!file_cache_->Lookup(key, entry, png) || png.size == 0) {
        return;
    }

    const auto filename = file_dialog_service::SavePngFileDialog(hwnd_, L"diagram.png");
    if (filename.empty()) {
        return;
    }

    if (WriteAllBytes(filename.c_str(), png.data.get(), png.size)) {
        show_toast_(i18n::S().toast_image_saved);
    }
    else {
        DeleteFileW(filename.c_str());
        show_toast_(i18n::S().toast_image_save_failed);
    }
}

void ClipboardManager::CopyDiagramAsSvg(const Document& doc, int node_index, float md_width, bool dark)
{
    const Node* node_ptr = NodeAt(doc, node_index);
    if (!node_ptr) {
        return;
    }
    const auto& node = *node_ptr;
    if (node.type != NodeType::CodeBlock || !IsSvgExportable(node.code_language)) {
        return;
    }
    if (svg_copy_in_flight_) {
        return;
    }
    if (!mermaid_renderer_) {
        return;
    }

    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    if (const auto* hit = svg_cache_.Find(key)) {
        const bool ok = WriteClipboardSvg(hwnd_, *hit);
        show_toast_(ok ? i18n::S().toast_svg_copied : i18n::S().toast_svg_copy_failed);
        return;
    }

    show_toast_(i18n::S().toast_svg_copying);
    svg_copy_in_flight_ = true;

    // RequestSvg は WebView2 経路のため wstring。UTF-8 → wide に変換して渡す。
    std::pmr::wstring code_wide;
    string_convert::Utf8ToWide(node.GetText(), code_wide);
    const std::wstring_view code_view = code_wide;
    mermaid_renderer_->RequestSvg(code_view, md_width, dark, [this, key](std::pmr::wstring svg, bool cancelled) {
        svg_copy_in_flight_ = false;
        if (cancelled) {
            // テーマ変更/幅変更/シャットダウン等によるキャンセル。トーストは出さない。
            return;
        }
        if (svg.empty()) {
            show_toast_(i18n::S().toast_svg_copy_failed);
            return;
        }
        const bool ok = WriteClipboardSvg(hwnd_, svg);
        if (ok) {
            svg_cache_.Insert(key, std::move(svg));
        }
        show_toast_(ok ? i18n::S().toast_svg_copied : i18n::S().toast_svg_copy_failed);
    });
}
