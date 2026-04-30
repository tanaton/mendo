#pragma once
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <map>
#include <atomic>

class TaskScheduler;

// Mermaidダイアグラムの描画済みPNGをファイルに永続化するキャッシュ。
// 幅は100px単位に量子化し、コード・テーマの組み合わせごとにPNGを保持する。
// PNG書き出しはTaskSchedulerを介してバックグラウンドスレッドで非同期に行う。
// インデックスはメモリ上で管理し、SaveIndex()でディスクに書き出す。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある
// （StoreAsyncのタスク投入のみスレッドセーフ）。
class MermaidFileCache {
public:
    MermaidFileCache() = default;
    ~MermaidFileCache();

    MermaidFileCache(const MermaidFileCache&) = delete;
    MermaidFileCache& operator=(const MermaidFileCache&) = delete;

    // Lookup結果のメタデータ
    struct CacheEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
    };

    // サイズ付きバイトバッファ
    struct PngBlob {
        std::unique_ptr<uint8_t[]> data;
        size_t size = 0;
    };

    void Init(float current_dpr, TaskScheduler& scheduler);
    bool Lookup(uint64_t key, CacheEntry& entry, PngBlob& png);
    bool LookupDimensions(uint64_t key, CacheEntry& entry) const noexcept;
    void StoreAsync(uint64_t key, float css_width, float css_height, std::pmr::vector<uint8_t> png_data);
    void SaveIndex();
    void ClearAll();
    void Shutdown();
    void SetCacheDir(const std::filesystem::path& dir);
    void SetLimits(size_t max_entries, uint64_t max_total_size);

    size_t EntryCount() const noexcept
    {
        return index_.size();
    }
    uint64_t TotalSize() const noexcept
    {
        return total_size_;
    }

#ifdef MENDO_TESTING
    // テスト用: 外部キーから実ファイル名（PNG）を導く。
    // 内部で DPR を mix した内部キーを使うため。
    constexpr uint64_t InternalKeyForTest(uint64_t external_key) const noexcept
    {
        return InternalKey(external_key);
    }
#endif

private:
    static constexpr uint32_t MAGIC = 0x4D454D43u; // "MEMC"
    static constexpr uint32_t VERSION = 1;
    static constexpr size_t DEFAULT_MAX_ENTRIES = 4096;
    static constexpr uint64_t DEFAULT_MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB

    using LruOrder = std::pmr::multimap<int64_t, uint64_t>;

    struct IndexEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
        uint32_t png_size = 0;
        int64_t last_used = 0;
        LruOrder::iterator lru_iter{};
    };

    std::filesystem::path GetCacheDir() const;
    std::filesystem::path GetPngPath(const std::filesystem::path& dir, uint64_t key) const;
    std::filesystem::path GetPngPath(uint64_t key) const;
    std::filesystem::path GetIndexPath() const;
    void LoadIndex();
    void EvictIfNeeded(uint32_t new_png_size);
    void DecrementTotalSize(uint32_t png_size) noexcept;
    static int64_t Now() noexcept;

    // current_dpr_ を mix した内部キーを返す。DPR ごとにエントリを分離して
    // DPI 変更/モニタ切替時の全消去を避ける。
    constexpr uint64_t InternalKey(uint64_t external_key) const noexcept
    {
        // DPR を 1/100 単位で量子化（典型値 100/125/150/175/200）。
        const uint32_t dpr_q = static_cast<uint32_t>(current_dpr_ * 100.0f + 0.5f);
        return external_key ^ (static_cast<uint64_t>(dpr_q) * 0x9E3779B97F4A7C15ULL);
    }

    std::filesystem::path cache_dir_;
    float current_dpr_ = 0.0f;

    std::pmr::unordered_map<uint64_t, IndexEntry> index_;
    LruOrder lru_order_;
    uint64_t total_size_ = 0;

    size_t max_entries_ = DEFAULT_MAX_ENTRIES;
    uint64_t max_total_size_ = DEFAULT_MAX_TOTAL_SIZE;

    // バックグラウンド書き込み
    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> write_gen_{ 0 };

    mutable std::mutex pending_mutex_;
    std::pmr::unordered_set<uint64_t> pending_writes_;
};
