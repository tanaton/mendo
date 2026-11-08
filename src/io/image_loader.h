#pragma once
#include "utility.h"
#include "worker_latch.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

class TaskScheduler;

// ImageLoader::ProcessCompletedDecodes で GPU に載せたビットマップ。TakeDelivered で受け取った側が描く側へ渡す。
class DeliveredImages {
public:
    ID2D1Bitmap* Find(const std::wstring& abs_path) const noexcept;

private:
    friend class ImageLoader;
    // キーはパスのハッシュ。
    FlatHashMap<Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;
};

class ImageLoader {
public:
    ImageLoader() noexcept;
    ~ImageLoader();

    // デコード結果が届いたら hwnd へ msg_id を投げる (hwnd が null なら投げない)。
    void Init(IWICImagingFactory* wic, HWND hwnd, UINT msg_id, TaskScheduler& scheduler);
    // **UI スレッドからのみ呼び出すこと** (worker が読むのは max_bitmap_dim_ だけ)。
    // デバイスロストでの差し替えでは、旧 rt のビットマップを捨てる。デコードは WIC ビットマップを作るので rt に依らず
    // (縮める上限 max_bitmap_dim_ は始めるときに読む)、待ちのものも受け箱の結果も新しい rt に載せる。
    void SetRenderTarget(ID2D1RenderTarget* rt);

    // 読み込んだ画像の寸法。ビットマップはここに残さず、描く側 (InlineDrawings) が保持範囲の画像の分だけ持つ。
    // 外れた画像は戻ったときにファイルから読み直す。ネットワークは読まないので、1705x1056 の PNG を 1024px で 5〜9ms、
    // 頼んでから出るまで 6〜7ms (1 フレーム以内)。ビットマップを 128 件・128MB まで残すキャッシュがあった頃は、
    // スクリーンショット 40 枚の文書を最後まで見ると private 130MB・GPU 110MB で、残さないと 52MB・30MB (2026-11-07)。
    struct ImageInfo {
        // 縮小前の原寸 (物理 px)。表示の DIP は DPI で変わるので DipSize で求める。
        float pixel_width = 0.0f;
        float pixel_height = 0.0f;
        // 最後に読んだビットマップは、この幅 (物理 px) までは縮小によるぼけ無しで描ける。原寸でデコードしていれば上限なし。
        // 寸法だけを読んだ (RequestSizeAsync) なら 0。
        UINT sharp_width_px = UINT_MAX;
    };
    // 次に寸法の表を変える (ProcessCompletedDecodes) まで有効。
    const ImageInfo* FindImageInfo(const std::wstring& abs_path) const noexcept;
    size_t ImageInfoCount() const noexcept
    {
        return infos_.size();
    }
    // 寸法の表の件数の上限。1 枠 24 バイト (4096 件で 200KB ほど) なので大きく取り、超えたら空にして数え直す。
    static constexpr size_t kMaxImageInfos = 4096;
    // 受け取ったビットマップを渡し、ImageLoader には残さない。
    DeliveredImages TakeDelivered() noexcept
    {
        return std::exchange(delivered_, {});
    }
    // 今の DPI での表示寸法 (画像の 1 px を物理 1 px で描く)。
    D2D1_SIZE_F DipSize(const ImageInfo& image) const noexcept;
    // max_width_dip: 表示幅。これを量子化した物理 px まで縮小してデコードする。0 は原寸。
    // 結果を待つ間は、取り込みのたびに頼み直さないと CancelStaleBitmapRequests で取り消される。
    void RequestLoadAsync(const std::wstring& abs_path, float max_width_dip);
    // 寸法だけを読む (ピクセルはデコードせず、ビットマップも作らない)。保持範囲の外の画像を、ビットマップを持たずに組めるようにする。
    // 待つ間に RequestLoadAsync が来たら、届いたときに読み直させる (ProcessCompletedDecodes の戻り値)。
    void RequestSizeAsync(const std::wstring& abs_path);
    // 読み込みに kMaxImageRetries 回失敗し、もう読まない (ResetFailedPaths まで)。
    bool HasGivenUp(const std::wstring& abs_path) const noexcept
    {
        const auto it = failed_paths_.find(abs_path);
        return it != failed_paths_.end() && it->second >= kMaxImageRetries;
    }
    // 表示幅が広がり、最後に読んだ解像度では足りなくなったか。
    bool NeedsSharperDecode(const ImageInfo& image, float max_width_dip) const noexcept
    {
        return DecodeWidthPx(max_width_dip) > image.sharp_width_px;
    }
    // deadline を過ぎた分は残し、HasLeftoverUploads が真になる。
    // 戻り値: 見えている画像を要求し直すべきか。結果を 1 件以上取り込んだ (失敗も含む) か、捨てた要求が待ちの間に要求し直されていた。
    bool ProcessCompletedDecodes(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    // 前に呼んでから RequestLoadAsync で頼み直されなかったビットマップの要求 (範囲を出た画像) を取り消す。取り込みのたびに、
    // まだ要るビットマップを頼み直させてから呼ぶ。取り消した要求は、worker が始める前なら読まずに返し、デコードの後なら GPU に
    // 載せず寸法だけを入れる。速いスクロールで待ち行列に溜まった要求を読み続けると、止まった位置の画像が後回しになる
    // (写真 200 枚の表を 16ms おきにめくると、止まってから画像が揃うまで 404 → 283ms、GPU への転送 133 → 76 回。2026-11-08)。
    void CancelStaleBitmapRequests() noexcept;
    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverUploads() const noexcept
    {
        return render_target_ && completed_.HasLeftover();
    }
    // 始まっていないデコードを worker に空振りさせる。始まったものの結果は寸法の表に入れる (パスがキーなので文書をまたいでも使える)。
    // どちらも結果が届くまで要求中のまま残し、同じパスを二重に読まない。
    void DropQueued();
    // DropQueued に加え、届いている結果と失敗の記録も捨てる。
    void CancelPending();

    void ResetFailedPaths()
    {
        failed_paths_.clear();
    }

    void Shutdown();

    // 寸法の表と受け取ったビットマップを空にする。テスト専用。
    void ClearImagesForTest() noexcept
    {
        infos_.Clear();
        delivered_ = {};
    }
    // 読み込んだ結果として寸法 (と bitmap があれば受け取ったビットマップ) を入れる。テスト専用。
    void InsertImageForTest(const std::wstring& path, float pixel_width, float pixel_height, ID2D1Bitmap* bitmap = nullptr)
    {
        StoreResult(path, { .pixel_width = pixel_width, .pixel_height = pixel_height }, bitmap);
    }

private:
    // 始まる前に捨てたか取り消したので、読まずに返した。
    struct Skipped {};
    // 寸法だけを読んだ。読めなければ pixel_width が 0。
    struct SizeRead {
        ImageInfo info;
    };
    // デコードし、縮小まで済ませた。読めなければ bitmap が null。
    struct Decoded {
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        ImageInfo info;
    };
    struct DecodeResult {
        std::wstring path;
        std::variant<Skipped, SizeRead, Decoded> outcome;
    };

    void StoreResult(const std::wstring& path, const ImageInfo& info, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap);
    float DpiScale() const noexcept;
    // 0 は無制限。幅の細かな変化で再デコードしないよう量子化する。
    UINT DecodeWidthPx(float width_dip) const noexcept;
    void QueueDecode(const std::wstring& abs_path, UINT max_width_px, bool size_only);
    // worker で、EnsureWorkerCom の後に呼ぶ。
    SizeRead ReadSize(const std::wstring& path) const;
    Decoded DecodeForDisplay(const std::wstring& path, UINT max_width_px) const;

    static constexpr uint32_t kMaxImageRetries = 3;
    // 圧縮済み画像のファイルサイズ上限。デコードはワーカー数ぶん並行するので、Markdown と同じ 2GB だと
    // 同時に数 GB を読み込ませられる。
    static constexpr LONGLONG MAX_IMAGE_FILE_SIZE = 256LL * 1024 * 1024;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    // worker が縮小サイズ計算に使う。render_target_ は worker から触れないので値で持つ。
    std::atomic<UINT> max_bitmap_dim_;
    // キーはパスのハッシュ。
    FlatHashMap<ImageInfo> infos_;
    DeliveredImages delivered_;

    HWND hwnd_ = nullptr;
    UINT msg_id_ = 0;
    TaskScheduler* scheduler_ = nullptr;
    // DropQueued で進める。投入時の値から変わっていたら worker は読まずに Skipped で返す (Mermaid の drop_gen_ と同じ)。
    std::atomic<uint32_t> drop_gen_{ 0 };
    // 寸法だけを読んでいる。取り消さない。
    struct ReadingSize {};
    // ビットマップを読んでいる。
    struct ReadingBitmap {
        // worker が始める前に見る取り消し。
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
        // 前の CancelStaleBitmapRequests の後に頼み直された。
        bool renewed = true;
        // 前の CancelStaleBitmapRequests の後に寸法だけを頼まれた (範囲を出た)。取り消しても寸法を待つ側が残る。
        bool size_wanted = false;
    };
    struct PendingDecode {
        // 始めた要求の種類。待つ間に別の種類を頼まれても変えない (寸法を待つ間のビットマップは、届いてから頼み直させる)。
        std::variant<ReadingSize, ReadingBitmap> reading;
        // 待つ間に要求し直された。読まずに返ったら要求し直させる。文書を切り替えて捨てた要求の結果で旧文書の画像を
        // 読み直さないよう、要求し直されたものだけ読み直させる。取り消したら、寸法だけを頼まれていたときだけ残す。
        bool requested_again = false;
    };
    // 結果を待っているパス。要求は必ず結果を 1 回返すので、届いたら外す。
    std::unordered_map<std::wstring, PendingDecode> pending_paths_;
    std::unordered_map<std::wstring, uint32_t> failed_paths_;

    PostedInbox<DecodeResult> completed_;

    // Shutdown で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
