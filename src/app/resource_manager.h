#pragma once
#include "app_constants.h"
#include "file_watcher.h"
#include "layout_computer.h"
#include "utility.h"
#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct AppState;
struct InlineDrawings;
struct InlineImageEntry;
class DeliveredImages;
class ImageLoader;
class IMermaidRenderer;
class InlineImageTable;
class MermaidFileCache;

// ResourceManager が App に頼むこと。通知は取り込み (FlushPendingResources・LoadImages) の中から届くので、
// 組み直しと描き直しだけをさせ、取り込みに入り直させない。
class ResourceManagerHost {
public:
    virtual void SetTimer(app_timer::Id id, UINT ms) = 0;
    virtual void KillTimer(app_timer::Id id) = 0;
    // 図の高さが changed の範囲で変わった。見ている位置を保って Y を組み直し、再描画させる。
    virtual void RecomputeLayout(IndexRange changed) = 0;
    // 本文中の画像の寸法かビットマップが変わった。pending_resolved なら寸法を待って仮に組んだノードを、sizes_changed なら
    // 組んだ寸法が変わった画像 (DPI の変更・読めなかった画像が読めた) があるので画像を持つノードを組み直させる。
    // どちらでもなければ描き直させる。
    virtual void OnInlineImagesUpdated(bool pending_resolved, bool sizes_changed) = 0;
    // 別の文書を読み込んでいる。この間は表示中の文書の画像と図 (拡大表示の図も) を新たに頼まない (届いた画像は取り込む)。
    // 読み始めに待ちを捨てても、進行中の 1 件が終わるか捨てた要求が読まずに戻ると次の取り込みが頼み直し、開いた文書の
    // 最初の図と画像を旧文書の描画・デコード (始まると取り消せない) の後回しにしていた。
    // 差し替えずに終わったら ScheduleBitmapManage で頼み直す (ReloadCommand::resume_resources)。
    virtual bool IsLoadingOtherDocument() const = 0;

protected:
    ~ResourceManagerHost() = default;
};

// 画像・Mermaidリソースのライフサイクル管理。
// Appから画像読み込み、Mermaidの要求、ビットマップ解放の責務を分離する。
class ResourceManager {
public:
    // 遅延レイアウトで計測する範囲。範囲外へのスクロールは EnsureVisibleLayout が同期計測で拾う。
    static constexpr float DEFERRED_LAYOUT_BUFFER_SCREENS = 2.0f;
    // 画像を読む範囲。遅延レイアウトの範囲より広くしても、組んでいない表は行の位置が分からず寸法しか読めない。
    static constexpr float PREFETCH_BUFFER_SCREENS = 2.0f;
    static_assert(PREFETCH_BUFFER_SCREENS <= DEFERRED_LAYOUT_BUFFER_SCREENS);
    // 画像のビットマップを残す範囲。読む範囲より狭いと、evict 直後の flush で範囲外の画像を毎回読み直す。
    // 読む範囲と同じ 2 画面だと、境目の画像を小刻みなスクロールのたびに捨てて読み直す。写真 40 枚の表で矢印キー 5 回の
    // 上下を 8 か所で 3 往復ずつすると、デコードが 76 → 35 回 (2.5 でも 3 でも同じ)。最後までめくって戻ったときの
    // 増え分は、2.5 で private +2%・GPU +2〜10%、3 ではもう少し増える (2026-11-08)。
    static constexpr float EVICT_BUFFER_SCREENS = 2.5f;
    static_assert(PREFETCH_BUFFER_SCREENS < EVICT_BUFFER_SCREENS);
    // text_layout と表のセルを残す範囲。先読み範囲と同じだと、少し戻るだけで作り直しになる。
    // 文書の大きさによらず上下 8 画面 (5 → 2 → 8 と揺れた末に決定)。1MB の文書を最後まで読んで測ると、
    // 8 画面と 2 画面ではメモリに差が出なかった (プライベートワーキングセット 63〜84MB)。
    // 1MB 以下の文書で全部残すのは、物理メモリ +50MB (全体の約 4 割) かかったのでやめた。
    static constexpr float LAYOUT_EVICT_BUFFER_SCREENS = 8.0f;
    static_assert(EVICT_BUFFER_SCREENS < LAYOUT_EVICT_BUFFER_SCREENS);

    // 渡すものはどれも ResourceManager より長生きする。inline_images と inline_drawings.images は本文中の画像の寸法と
    // ビットマップで、ResourceManager が読み込みの結果を書く。
    ResourceManager(AppState& state, ImageLoader& image_loader, IMermaidRenderer& mermaid, MermaidFileCache& mermaid_file_cache,
                    InlineImageTable& inline_images, InlineDrawings& inline_drawings, ResourceManagerHost& host) noexcept
        : state_(state), image_loader_(image_loader), mermaid_(mermaid), mermaid_file_cache_(mermaid_file_cache),
          inline_images_(inline_images), inline_drawings_(inline_drawings), host_(host)
    {}

    // range のノードの本文中の画像に、読んだことのある画像の寸法を入れる。読んでいない画像は無視。
    // 組む前に呼べば最初から画像の寸法で組める。寸法を入れたら true。読み込みは後続の FlushPendingResources が起動する。
    bool ApplyCachedImageSizes(IndexRange range);
    // 開くときとリロードのスクロール位置の計算用に、range のノードの推定高さを図のキャッシュにある実測の寸法で上書きし、
    // 読んだことのある本文中の画像の寸法も入れる。高さを変えたら true (Y は組み直さない)。
    // 呼び出し前にノード高さの初期化 (EstimateNodeHeights) は済ませておく。
    bool ApplyCachedHeights(IndexRange range);
    void LoadImages();
    // 届いたデコード結果を取り込み、あれば適用して再レイアウトを 1 回行う。
    void OnImagesDecoded(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);

    void RequestMermaidRenders();

    void EvictOffscreenBitmaps();
    // 届いたデコード結果を取り込んで先読み範囲の画像に入れ、図の描画を頼む。
    void FlushPendingResources();
    void ScheduleBitmapManage();
    void OnBitmapManageTimer();
    // 図の描画待ちと画像のデコード待ちを捨てる (描画中の分は残る)。
    void DropQueues();
    // 開いた文書の画像は src を前の文書のフォルダで解決したものなので、寸法とビットマップを捨てる。解決したパスと監視する
    // 画像も前の文書の分なので外す。フォルダが変わったときだけ外していた頃は、同じフォルダの文書を渡り歩くと前の文書の
    // 画像のフォルダが監視の枠 (ImageFolderWatcher::kMaxFolders) を占め続け、使い切った後の文書では更新が出なかった。
    // 2026-11-11 からは開くたびにフォルダの監視ごと捨てていたが、同じフォルダの文書を渡り歩くたびに開き直すので、
    // 2026-11-13 にフォルダの読み取りは残し、枠が埋まったら今の文書で使っていないものから前に使った順にやめる形にした
    // (ユーザーの判断)。
    void ForgetInlineImages();
    // 旧デバイスのビットマップと図のコマンドリストを捨て、新しい RT で読み直す。
    void OnRenderDeviceRecreated();

    // 本文中の画像のファイルが更新されたら読み直す。監視するのは src を解決した (先読み範囲に入ったことのある) 画像で、
    // 別の文書を開くまで続ける。フォルダは worker で開き、開き終えるまでそのフォルダの画像は読まない
    // (読んでから監視を始めるまでの更新を取りこぼさない)。
    // メッセージループが待つ、画像のフォルダの監視のイベント。out は kMaxImageWatchEvents 以上。数を返す。
    static constexpr size_t kMaxImageWatchEvents = ImageFolderWatcher::kMaxEvents;
    size_t ImageWatchEvents(std::span<HANDLE> out) const noexcept
    {
        return image_watcher_.EventHandles(out);
    }
    // 画像のフォルダを開いている最中 (そのフォルダの画像をまだ頼んでいない) か。
    bool IsOpeningImageFolders() const noexcept
    {
        return image_watcher_.AwaitsWorker();
    }
    // ImageWatchEvents のどれかが立った。更新された画像を控え、IMAGE_RELOAD タイマーを掛ける。フォルダを開き終えていたら、
    // 待たせていた画像を読ませる。
    void OnImageFolderEvent();
    // 控えた画像を読み直させる。
    void OnImageReloadTimer();

private:
    // range のノードの図の推定高さをディスクキャッシュにある寸法で上書きする。
    // 描画命令は作らないので、描画は App の後始末 (FinishLayout) の FlushPendingResources に任せる。
    bool ApplyMermaidCacheHeights(IndexRange range);
    // 本文中の画像の表の変わり方。NotifyInlineImages で App に組み直しか描き直しをさせる。
    struct InlineImageUpdate {
        bool pending_resolved = false;
        bool sizes_changed = false;
        bool bitmaps_changed = false;
        // 行の位置が決まらないので、寸法だけを読んだ表があった。
        bool unsettled_tables = false;
    };
    // 以下の適用と要求は再レイアウトしない。画像は update に、高さが変わった図のノードは changed に積む。
    // slices は走査する画像を持つノードの範囲の並び。delivered があれば受け取ったビットマップを描く側へ渡し、描くビットマップの
    // 無い画像の読み込みを起動する。表は先読み範囲の行の画像だけビットマップを読み、ほかの行の画像は寸法だけを読む。
    // delivered が無ければ寸法だけを入れる。
    void ApplyCachedImagesIn(std::span<const std::span<const size_t>> slices, const DeliveredImages* delivered, InlineImageUpdate& update);
    // max_width はデコードする幅 (0 は原寸)。needs_bitmap が偽なら、寸法が無いときに寸法だけを読む。
    void ApplyCachedImage(std::string_view src, const std::wstring& doc_dir, float max_width, const DeliveredImages* delivered, bool needs_bitmap,
                          InlineImageUpdate& update);
    void SetInlineImage(uint64_t key, const InlineImageEntry& entry, InlineImageUpdate& update);
    void NotifyInlineImages(const InlineImageUpdate& update);
    void RequestVisibleDiagrams(IndexRange& changed);
    void RequestDiagramRender(size_t i, float content_width, bool dark_mode, IndexRange& changed);
    // 解決済みパスは src 単位でキャッシュする。開かない src も空文字列で覚え、毎回の解決を避ける。
    const std::wstring* ResolveImagePath(std::string_view src, const std::wstring& doc_dir);
    // 保持範囲 (±EVICT_BUFFER_SCREENS) を越えて移動していたら、旧位置で積んだ図の描画待ちと画像のデコード待ちを捨てる。
    // FIFO のままだと TOC ジャンプや Ctrl+End の後に、見えている図や画像が旧位置の分の後回しになる。
    void DropQueuesIfJumped();

    struct BufferedRange {
        float viewport_top;
        float viewport_bottom;
        YRange keep;
        IndexRange range;
    };
    // 可視範囲 ± viewport_height * screens に交差するノード範囲。viewport_height <= 0 (初期化中等) は範囲が決まらない。
    std::optional<BufferedRange> ComputeBufferedRange(float screens);
    // indices のうち先読み範囲 (PREFETCH_BUFFER_SCREENS) に交差する部分を、可視部分 → 下側 → 上側の順の 3 区間に分ける。
    // 範囲が決まらないときは全件を可視部分として返す。要求キューは FIFO なので、
    // スクロール位置の復元やジャンプの直後に、画面より上の分が見えている分より先に処理されないようにする。
    std::array<std::span<const size_t>, 3> PrioritizedSlices(std::span<const size_t> indices);
    // 前回の要求から量子化幅かテーマが変わっていれば、描き直す図を stale にして旧条件の描画待ちを捨てる。
    void InvalidateDiagramsForRequestChange(float content_width, bool dark_mode);

    AppState& state_;
    ImageLoader& image_loader_;
    IMermaidRenderer& mermaid_;
    MermaidFileCache& mermaid_file_cache_;
    InlineImageTable& inline_images_;
    InlineDrawings& inline_drawings_;
    ResourceManagerHost& host_;

    float last_mermaid_content_width_ = 0.0f;
    bool last_mermaid_dark_mode_ = false;
    float last_request_scroll_ = 0.0f;
    std::unordered_map<std::string, std::wstring, StringTransparentHash, std::equal_to<>> resolved_image_paths_;
    std::wstring resolved_dir_;
    // resolved_image_paths_ に入れた画像のファイルの監視と、更新の通知が来て IMAGE_RELOAD タイマーを待っているパス。
    ImageFolderWatcher image_watcher_;
    std::vector<std::wstring> changed_image_paths_;
    std::chrono::steady_clock::time_point last_flush_time_{};
};
