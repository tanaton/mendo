#include "clipboard_manager.h"
#include "clipboard_util.h"
#include "document.h"
#include "file_dialog_service.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_file_cache.h"
#include "mermaid_util.h"
#include "selection_html.h"
#include "stream_util.h"
#include "string_convert.h"
#include "wic_util.h"
#include <memory_resource>
#include <vector>

namespace {

// 範囲内かつ CodeBlock であれば node を返す。
const Node* ValidateCodeBlockNode(const Document& doc, int node_index) noexcept
{
    const auto& nodes = doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return nullptr;
    }
    const Node& node = nodes[node_index];
    if (node.type != NodeType::CodeBlock) {
        return nullptr;
    }
    return &node;
}

} // namespace

void ClipboardManager::Init(HWND hwnd, MermaidFileCache* file_cache, IMermaidRenderer* mermaid_renderer, IWICImagingFactory* wic, ToastCallback show_toast) noexcept
{
    hwnd_ = hwnd;
    file_cache_ = file_cache;
    mermaid_renderer_ = mermaid_renderer;
    wic_ = wic;
    show_toast_ = std::move(show_toast);
}

void ClipboardManager::CopyCodeBlock(const Document& doc, int node_index, bool dark) const
{
    const Node* node = ValidateCodeBlockNode(doc, node_index);
    if (!node) {
        return;
    }

    const std::pmr::string html_utf8 = BuildCodeBlockHtmlFragment(*node, dark);
    WriteClipboardHtml(hwnd_, html_utf8, node->GetText());
}

void ClipboardManager::SaveDiagramAsPng(const Document& doc, int node_index, float md_width, bool dark)
{
    const Node* node_ptr = ValidateCodeBlockNode(doc, node_index);
    if (!node_ptr || !IsDiagramLanguage(node_ptr->code_language())) {
        return;
    }
    const auto& node = *node_ptr;
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

    // 既存ファイルを上書き選択した場合でも、失敗時に原本を破壊しないよう
    // tmp+rename のアトミック書き込みを使う。
    if (AtomicWriteAllBytes(filename.c_str(), png.data.get(), png.size)) {
        show_toast_(i18n::S().toast_image_saved);
    }
    else {
        show_toast_(i18n::S().toast_image_save_failed);
    }
}

UniqueGlobalMem ClipboardManager::BuildDib(const PngBytes& png) const
{
    if (!png || png->empty() || !wic_) {
        return {};
    }
    auto stream = stream_util::CreateMemoryStream(png->data(), png->size());
    if (!stream) {
        return {};
    }
    // BI_RGB の DIB なので非プリマルチプライの 32bpp BGRA でデコードする。
    auto decoded = wic_util::DecodeFromStream(wic_, stream.Get(), GUID_WICPixelFormat32bppBGRA);
    if (!decoded || decoded->pixel_width == 0 || decoded->pixel_height == 0) {
        return {};
    }
    const UINT w = decoded->pixel_width;
    const UINT h = decoded->pixel_height;
    const size_t total = DibTotalBytes(w, h);
    if (total == 0) {
        return {};
    }
    UniqueGlobalMem mem{ GlobalAlloc(GMEM_MOVEABLE, total) };
    if (!mem) {
        return {};
    }
    auto* base = static_cast<uint8_t*>(GlobalLock(mem.get()));
    if (!base) {
        return {};
    }
    WriteDibHeader(base, w, h);
    const UINT stride = w * 4;
    const HRESULT hr = decoded->converter->CopyPixels(
        nullptr, stride, stride * h, base + sizeof(BITMAPINFOHEADER));
    GlobalUnlock(mem.get());
    if (FAILED(hr)) {
        return {};
    }
    return mem;
}

void ClipboardManager::EmitCopyResult(bool ok) const
{
    show_toast_(ok ? i18n::S().toast_diagram_copied : i18n::S().toast_diagram_copy_failed);
}

void ClipboardManager::CopyDiagramToClipboard(const Document& doc, int node_index, PngBytes png, float md_width, bool dark)
{
    const Node* node_ptr = ValidateCodeBlockNode(doc, node_index);
    if (!node_ptr || !IsDiagramLanguage(node_ptr->code_language())) {
        return;
    }
    const auto& node = *node_ptr;
    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    // 世代は「実際に書き込む経路」でのみ進める。in-flight で弾く再入では進めないことで、
    // 進行中リクエストのコールバックを誤って stale 化しない。同期コピー(LaTeX / SVG キャッシュ
    // ヒット)は保留中の非同期コールバックの上書きを防ぐため世代を進める。

    // LaTeX は flowchart ラッパなので SVG は意味を成さない。画像 (CF_DIB) のみを同期コピーする。
    if (!IsSvgExportable(node.code_language())) {
        ++copy_generation_;
        EmitCopyResult(WriteClipboardDiagram(hwnd_, BuildDib(png), {}));
        return;
    }

    // Mermaid は SVG + 画像。SVG は WebView2 経由の非同期取得のためキャッシュ優先。
    if (const auto* hit = svg_cache_.Find(key)) {
        ++copy_generation_;
        EmitCopyResult(WriteClipboardDiagram(hwnd_, BuildDib(png), *hit));
        return;
    }
    if (copy_in_flight_) {
        return;
    }
    if (!mermaid_renderer_) {
        return;
    }

    const uint64_t gen = ++copy_generation_;
    show_toast_(i18n::S().toast_diagram_copying);
    copy_in_flight_ = true;

    // 画像は同期で確定する。dib を move キャプチャするので SVG 取得完了時に再デコード不要。
    UniqueGlobalMem dib = BuildDib(png);

    // RequestSvg は WebView2 経路のため wstring。UTF-8 → wide に変換して渡す。
    std::pmr::wstring code_wide;
    string_convert::Utf8ToWide(node.GetText(), code_wide);
    const std::wstring_view code_view = code_wide;
    mermaid_renderer_->RequestSvg(code_view, md_width, dark,
        [this, key, gen, dib = std::move(dib)](std::pmr::wstring svg, bool cancelled) mutable {
            copy_in_flight_ = false;
            if (cancelled || gen != copy_generation_) {
                return;
            }
            const bool ok = WriteClipboardDiagram(hwnd_, std::move(dib), svg);
            if (ok && !svg.empty()) {
                svg_cache_.Insert(key, std::move(svg));
            }
            EmitCopyResult(ok);
        });
}
