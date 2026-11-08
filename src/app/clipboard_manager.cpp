#include "clipboard_manager.h"
#include "clipboard_util.h"
#include "document.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_util.h"
#include "selection_html.h"
#include "wic_util.h"
#include <objidl.h>

namespace {

// source を 32bpp BGRA の PNG にして path に書く。
bool SavePng(IWICImagingFactory* wic, IWICBitmapSource* source, const std::filesystem::path& path)
{
    const auto bgra = wic_util::ConvertBitmapSource(wic, source, GUID_WICPixelFormat32bppBGRA);
    Microsoft::WRL::ComPtr<IStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    UINT width = 0;
    UINT height = 0;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    STATSTG stat{};
    HGLOBAL mem = nullptr;
    if (!bgra || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || FAILED(bgra->GetSize(&width, &height)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(width, height)) ||
        FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA || FAILED(frame->WriteSource(bgra.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()) || FAILED(stream->Stat(&stat, STATFLAG_NONAME)) ||
        FAILED(GetHGlobalFromStream(stream.Get(), &mem))) {
        return false;
    }
    const auto size = static_cast<size_t>(stat.cbSize.QuadPart);
    const void* data = size != 0 && size <= GlobalSize(mem) ? GlobalLock(mem) : nullptr;
    if (!data) {
        return false;
    }
    // 既存ファイルを上書き選択した場合でも、失敗時に原本を破壊しないよう
    // tmp+rename のアトミック書き込みを使う。
    const bool ok = AtomicWriteUserFile(path, data, size);
    GlobalUnlock(mem);
    return ok;
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
    const Node* node = NodeOfType(doc.GetNodes(), node_index, NodeType::CodeBlock);
    if (!node) {
        return;
    }

    // 区切ったコードブロックは並び全体をコピーする。選択コピーと同じく、プレーンテキストの改行は CRLF にする。
    const auto& nodes = doc.GetNodes();
    const auto last = static_cast<int>(ChunkRunLast(nodes, static_cast<size_t>(node_index)));
    const auto selection = TextSelection::MakeOrdered(node_index, 0, last, static_cast<uint32_t>(nodes[last].GetText().size()));
    WriteClipboardHtml(hwnd_, ExtractSelectedTextAsHtml(nodes, selection, dark), ExtractSelectedText(nodes, selection));
}

void ClipboardManager::SaveDiagramAsPng(const Document& doc, int node_index, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing) const
{
    if (!NodeOfType(doc.GetNodes(), node_index, NodeType::Diagram) || !drawing) {
        return;
    }

    const auto filename = file_dialog::SavePngFile(hwnd_, L"diagram.png");
    if (filename.empty()) {
        return;
    }

    const auto bitmap = mermaid_renderer_->DiagramBitmap(*drawing);
    const bool ok = bitmap && SavePng(wic_, bitmap.Get(), filename);
    show_toast_(ok ? i18n::S().toast_image_saved : i18n::S().toast_image_save_failed);
}

UniqueGlobalMem ClipboardManager::BuildDib(IWICBitmapSource* bitmap) const
{
    if (!bitmap) {
        return {};
    }
    // BI_RGB の DIB なので非プリマルチプライの 32bpp BGRA にする。
    const auto bgra = wic_util::ConvertBitmapSource(wic_, bitmap, GUID_WICPixelFormat32bppBGRA);
    UINT w = 0;
    UINT h = 0;
    if (!bgra || FAILED(bgra->GetSize(&w, &h)) || w == 0 || h == 0) {
        return {};
    }
    const size_t total = DibTotalBytes(w, h);
    if (total == 0) {
        return {};
    }
    return AllocGlobalFilled(total, [&](void* p) {
        auto* base = static_cast<uint8_t*>(p);
        WriteDibHeader(base, w, h);
        const UINT stride = w * 4;
        return SUCCEEDED(bgra->CopyPixels(nullptr, stride, stride * h, base + sizeof(BITMAPINFOHEADER)));
    });
}

void ClipboardManager::EmitCopyResult(bool ok) const
{
    show_toast_(ok ? i18n::S().toast_diagram_copied : i18n::S().toast_diagram_copy_failed);
}

void ClipboardManager::CopyDiagramToClipboard(const Document& doc, int node_index, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing,
                                              float md_width, bool dark)
{
    const Node* node_ptr = NodeOfType(doc.GetNodes(), node_index, NodeType::Diagram);
    if (!node_ptr) {
        return;
    }
    // 画像はコピーを始めるときだけ描く (コピー中の再入では描かない)。
    const auto build_dib = [&] {
        const auto bitmap = drawing ? mermaid_renderer_->DiagramBitmap(*drawing) : nullptr;
        return BuildDib(bitmap.Get());
    };
    const auto& node = *node_ptr;
    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);

    // SVG + 画像。SVG は bg のワーカーで描く非同期の取得なのでキャッシュ優先。
    if (const auto* hit = svg_cache_.Find(key)) {
        // 待っているコピーの結果で、後から押したこのコピーを上書きさせない。
        pending_copy_.reset();
        EmitCopyResult(WriteClipboardDiagram(hwnd_, build_dib(), *hit));
        return;
    }
    if (pending_copy_ && pending_copy_->key == key) {
        return;
    }
    // 別の図を待っていたら、後から押したこちらに置き換える。
    const uint64_t number = ++copy_requests_;
    pending_copy_ = PendingCopy{ number, key };
    show_toast_(i18n::S().toast_diagram_copying);

    // 画像は同期で確定し、SVG が届くまで dib で持つ。
    UniqueGlobalMem dib = build_dib();

    // clang-format off
    mermaid_renderer_->RequestSvg(
        node.GetText(),
        md_width,
        dark,
        [this, key, number, dib = std::move(dib)](std::string_view svg) mutable {
            if (!pending_copy_ || pending_copy_->number != number) {
                return;
            }
            pending_copy_.reset();
            const bool ok = WriteClipboardDiagram(hwnd_, std::move(dib), svg);
            if (ok && !svg.empty()) {
                svg_cache_.Insert(key, std::string{ svg });
            }
            EmitCopyResult(ok);
        }
    );
    // clang-format on
}
