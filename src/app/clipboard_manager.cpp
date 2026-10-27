#include "clipboard_manager.h"
#include "clipboard_util.h"
#include "document.h"
#include "file_dialog_service.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_util.h"
#include "selection_html.h"
#include "wic_util.h"

namespace {

const Node* ValidateNode(const Document& doc, int node_index, NodeType type) noexcept
{
    const auto& nodes = doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size()) || nodes[node_index].type != type) {
        return nullptr;
    }
    return &nodes[node_index];
}

} // namespace

void ClipboardManager::Init(HWND hwnd, IMermaidRenderer* mermaid_renderer, IWICImagingFactory* wic, ToastCallback show_toast) noexcept
{
    hwnd_ = hwnd;
    mermaid_renderer_ = mermaid_renderer;
    wic_ = wic;
    show_toast_ = std::move(show_toast);
}

void ClipboardManager::CopyCodeBlock(const Document& doc, int node_index, bool dark) const
{
    const Node* node = ValidateNode(doc, node_index, NodeType::CodeBlock);
    if (!node) {
        return;
    }

    // 区切ったコードブロックは並び全体をコピーする。選択コピーと同じく、プレーンテキストの改行は CRLF にする。
    const auto& nodes = doc.GetNodes();
    const auto last = static_cast<int>(ChunkRunLast(nodes, static_cast<size_t>(node_index)));
    const auto selection = TextSelection::MakeOrdered(node_index, 0, last, static_cast<uint32_t>(nodes[last].GetText().size()));
    WriteClipboardHtml(hwnd_, ExtractSelectedTextAsHtml(nodes, selection, dark), ExtractSelectedText(nodes, selection));
}

void ClipboardManager::SaveDiagramAsPng(const Document& doc, int node_index, const PngBytes& png) const
{
    if (!ValidateNode(doc, node_index, NodeType::Diagram) || !png || png->empty()) {
        return;
    }

    const auto filename = file_dialog_service::SavePngFileDialog(hwnd_, L"diagram.png");
    if (filename.empty()) {
        return;
    }

    // 既存ファイルを上書き選択した場合でも、失敗時に原本を破壊しないよう
    // tmp+rename のアトミック書き込みを使う。
    const bool ok = AtomicWriteAllBytes(filename.c_str(), png->data(), png->size());
    show_toast_(ok ? i18n::S().toast_image_saved : i18n::S().toast_image_save_failed);
}

UniqueGlobalMem ClipboardManager::BuildDib(const PngBytes& png) const
{
    if (!png || png->empty() || !wic_) {
        return {};
    }
    // BI_RGB の DIB なので非プリマルチプライの 32bpp BGRA でデコードする。
    auto decoded = wic_util::DecodeFromMemory(wic_, *png, GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA);
    if (!decoded || decoded->pixel_width == 0 || decoded->pixel_height == 0) {
        return {};
    }
    const UINT w = decoded->pixel_width;
    const UINT h = decoded->pixel_height;
    const size_t total = DibTotalBytes(w, h);
    if (total == 0) {
        return {};
    }
    return AllocGlobalFilled(total, [&](void* p) {
        auto* base = static_cast<uint8_t*>(p);
        WriteDibHeader(base, w, h);
        const UINT stride = w * 4;
        return SUCCEEDED(decoded->converter->CopyPixels(nullptr, stride, stride * h, base + sizeof(BITMAPINFOHEADER)));
    });
}

void ClipboardManager::EmitCopyResult(bool ok) const
{
    show_toast_(ok ? i18n::S().toast_diagram_copied : i18n::S().toast_diagram_copy_failed);
}

void ClipboardManager::CopyDiagramToClipboard(const Document& doc, int node_index, PngBytes png, float md_width, bool dark)
{
    const Node* node_ptr = ValidateNode(doc, node_index, NodeType::Diagram);
    if (!node_ptr) {
        return;
    }
    const auto& node = *node_ptr;
    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    // 世代は「実際に書き込む経路」でのみ進める。in-flight で弾く再入では進めないことで、
    // 進行中リクエストのコールバックを誤って stale 化しない。同期コピー(LaTeX / SVG キャッシュ
    // ヒット)は保留中の非同期コールバックの上書きを防ぐため世代を進める。

    // LaTeX は flowchart ラッパなので SVG は意味を成さない。画像 (CF_DIB) のみを同期コピーする。
    if (!IsSvgExportable(node.code_language)) {
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
    if (copy_in_flight_ || !mermaid_renderer_) {
        return;
    }

    const uint64_t gen = ++copy_generation_;
    show_toast_(i18n::S().toast_diagram_copying);
    copy_in_flight_ = true;

    // 画像は同期で確定する。dib を move キャプチャするので SVG 取得完了時に再デコード不要。
    UniqueGlobalMem dib = BuildDib(png);

    // clang-format off
    mermaid_renderer_->RequestSvg(
        node.GetText(),
        md_width,
        dark,
        [this, key, gen, dib = std::move(dib)](std::pmr::wstring svg, bool cancelled) mutable {
            copy_in_flight_ = false;
            if (gen != copy_generation_) {
                return;
            }
            // ズーム・リロード等で取り消された。「コピー中」のまま終わらせない。
            if (cancelled) {
                EmitCopyResult(false);
                return;
            }
            const bool ok = WriteClipboardDiagram(hwnd_, std::move(dib), svg);
            if (ok && !svg.empty()) {
                svg_cache_.Insert(key, std::move(svg));
            }
            EmitCopyResult(ok);
        }
    );
    // clang-format on
}
