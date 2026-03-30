#pragma once
#include <cstdint>
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

    // キャッシュを初期化し、インデックスをディスクから読み込む。
    // current_dprが保存済みDPRと異なる場合、キャッシュを全削除する。
    void Init(float current_dpr, TaskScheduler& scheduler);

    // キャッシュされたPNGを検索する。見つかった場合trueを返す。
    // last_usedタイムスタンプも更新される。
    bool Lookup(uint64_t key, CacheEntry& entry, std::vector<uint8_t>& png_data);

    // PNGファイルをバックグラウンドスレッドで非同期に書き出す。
    // インデックスエントリは即座に追加される。
    void StoreAsync(uint64_t key, float css_width, float css_height,
        std::vector<uint8_t> png_data);

    // インデックスをディスクに保存する（アプリ終了時に呼び出す）。
    void SaveIndex();

    // すべてのキャッシュファイルとインデックスを削除する。
    void ClearAll();

    // 保留中の書き込みタスクを無効化する。
    void Shutdown();

    // テスト用: キャッシュディレクトリを上書きする。Init()の前に呼び出す。
    void SetCacheDir(const std::filesystem::path& dir);

    // テスト用: エントリ数・サイズ上限を変更する。Init()の前に呼び出す。
    void SetLimits(size_t max_entries, uint64_t max_total_size);

    size_t EntryCount() const noexcept { return index_.size(); }
    uint64_t TotalSize() const noexcept { return total_size_; }

private:
    static constexpr uint32_t kMagic = 0x4D454D43u;   // "MEMC"
    static constexpr uint32_t kVersion = 1;
    static constexpr size_t kDefaultMaxEntries = 4096;
    static constexpr uint64_t kDefaultMaxTotalSize = 1ULL * 1024 * 1024 * 1024; // 1GB

    struct IndexEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
        uint32_t png_size = 0;
        int64_t last_used = 0;
    };

    std::filesystem::path GetCacheDir() const;
    std::filesystem::path GetPngPath(uint64_t key) const;
    std::filesystem::path GetIndexPath() const;
    void LoadIndex();
    void EvictIfNeeded(uint32_t new_png_size);
    void RemoveLruEntry(int64_t timestamp, uint64_t key);
    static int64_t Now();

    std::filesystem::path cache_dir_override_;
    float stored_dpr_ = 0.0f;
    float current_dpr_ = 0.0f;

    // インデックス: key → エントリメタデータ
    std::unordered_map<uint64_t, IndexEntry> index_;
    // LRU順序: last_used → keys（O(log n) での最古エントリ特定用）
    std::multimap<int64_t, uint64_t> lru_order_;
    uint64_t total_size_ = 0;

    size_t max_entries_ = kDefaultMaxEntries;
    uint64_t max_total_size_ = kDefaultMaxTotalSize;

    // バックグラウンド書き込み
    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> write_gen_{0};
};
