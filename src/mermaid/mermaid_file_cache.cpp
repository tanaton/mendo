#include "mermaid_file_cache.h"
#include "task_scheduler.h"
#include "file_io.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <chrono>
#include <cmath>

namespace {

#pragma pack(push, 1)
struct IndexHeader {
    uint32_t magic;
    uint32_t version;
    float dpr;
    uint32_t count;
};

struct IndexRecord {
    uint64_t key;
    float css_width;
    float css_height;
    uint32_t png_size;
    int64_t last_used;
};
#pragma pack(pop)

static_assert(sizeof(IndexHeader) == 16);
static_assert(sizeof(IndexRecord) == 28);

} // namespace

MermaidFileCache::~MermaidFileCache()
{
    Shutdown();
}

void MermaidFileCache::SetCacheDir(const std::filesystem::path& dir)
{
    cache_dir_ = dir;
}

void MermaidFileCache::SetLimits(size_t max_entries, uint64_t max_total_size)
{
    max_entries_ = max_entries;
    max_total_size_ = max_total_size;
}

std::filesystem::path MermaidFileCache::GetCacheDir() const
{
    return cache_dir_;
}

std::filesystem::path MermaidFileCache::GetPngPath(const std::filesystem::path& dir, uint64_t key) const
{
    wchar_t name[24];
    swprintf_s(name, L"%016llx.png", key);
    return dir / name;
}

std::filesystem::path MermaidFileCache::GetPngPath(uint64_t key) const
{
    const auto dir = GetCacheDir();
    if (dir.empty()) {
        return {};
    }
    return GetPngPath(dir, key);
}

std::filesystem::path MermaidFileCache::GetIndexPath() const
{
    const auto dir = GetCacheDir();
    if (dir.empty()) {
        return {};
    }
    return dir / L"index.bin";
}

int64_t MermaidFileCache::Now() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void MermaidFileCache::Init(float current_dpr, TaskScheduler& scheduler)
{
    scheduler_ = &scheduler;
    current_dpr_ = current_dpr;
    const auto dir = GetCacheDir();
    if (dir.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return;
    }

    LoadIndex();

    // DPR ごとに InternalKey() が別キーに振り分けるため、DPR 不一致でも消去せず
    // LRU で自然淘汰させる。
    stored_dpr_ = current_dpr;
}

void MermaidFileCache::LoadIndex()
{
    index_.clear();
    lru_order_.clear();
    total_size_ = 0;

    const auto path = GetIndexPath();
    if (path.empty()) {
        return;
    }

    auto [buf, buf_size] = ReadAllBytes(path);
    if (!buf || buf_size < 16) {
        return;
    }

    const uint8_t* p = buf.get();
    IndexHeader header;
    std::memcpy(&header, p, sizeof(header));
    p += sizeof(header);

    if (header.magic != MAGIC || header.version != VERSION) {
        return;
    }
    // 異常なエントリ数を拒否
    if (header.count > DEFAULT_MAX_ENTRIES * 2) {
        return;
    }

    stored_dpr_ = header.dpr;
    index_.reserve(header.count);

    for (uint32_t i = 0; i < header.count; ++i) {
        if (static_cast<size_t>(p - buf.get()) + sizeof(IndexRecord) > buf_size) {
            break;
        }

        IndexRecord record;
        std::memcpy(&record, p, sizeof(record));
        p += sizeof(record);

        // 壊れたエントリを無視する
        if (record.css_width <= 0.0f || record.css_height <= 0.0f ||
            !std::isfinite(record.css_width) || !std::isfinite(record.css_height) ||
            record.png_size == 0) {
            continue;
        }

        auto& entry_ref = index_[record.key];
        entry_ref.css_width = record.css_width;
        entry_ref.css_height = record.css_height;
        entry_ref.png_size = record.png_size;
        entry_ref.last_used = record.last_used;
        entry_ref.lru_iter = lru_order_.emplace(record.last_used, record.key);
        total_size_ += record.png_size;
    }
}

void MermaidFileCache::SaveIndex()
{
    const auto path = GetIndexPath();
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const uint32_t count = static_cast<uint32_t>(index_.size());

    const size_t buf_size = sizeof(IndexHeader) + count * sizeof(IndexRecord);
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(buf_size);
    uint8_t* p = buf.get();

    IndexHeader header{ MAGIC, VERSION, current_dpr_, count };
    std::memcpy(p, &header, sizeof(header));
    p += sizeof(header);

    for (const auto& [key, entry] : index_) {
        IndexRecord record{ key, entry.css_width, entry.css_height, entry.png_size, entry.last_used };
        std::memcpy(p, &record, sizeof(record));
        p += sizeof(record);
    }

    const auto tmp_path = path.parent_path() / L"index.bin.tmp";
    if (!WriteAllBytes(tmp_path, buf.get(), buf_size)) {
        return;
    }

    if (!MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // renameが失敗した場合、直接書き込み
        DeleteFileW(tmp_path.c_str());
        WriteAllBytes(path, buf.get(), buf_size);
    }
}

bool MermaidFileCache::Lookup(uint64_t key, CacheEntry& entry, PngBlob& png)
{
    key = InternalKey(key);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }

    const auto path = GetPngPath(key);
    if (path.empty()) {
        return false;
    }

    DWORD read_error = 0;
    auto [data, data_size] = ReadAllBytes(path, &read_error);
    if (!data) {
        // ファイルが確実に存在しない場合のみインデックスエントリを除去する。
        // 共有違反など一時的なエラーではエントリを保持する。
        // また、StoreAsync 直後でバックグラウンド書き込みが in-flight なら
        // 「未着地＝stale」と早合点せず entry を保持する（次回 Lookup で着地する）。
        if (read_error == ERROR_FILE_NOT_FOUND || read_error == ERROR_PATH_NOT_FOUND) {
            bool pending = false;
            {
                std::lock_guard lock(pending_mutex_);
                pending = pending_writes_.contains(key);
            }
            if (!pending) {
                DecrementTotalSize(it->second.png_size);
                lru_order_.erase(it->second.lru_iter);
                index_.erase(it);
            }
        }
        return false;
    }
    png.data = std::move(data);
    png.size = data_size;

    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;

    // last_usedを更新し、LRU順序を再配置（O(1) で erase/insert）
    const int64_t new_time = Now();
    lru_order_.erase(it->second.lru_iter);
    it->second.last_used = new_time;
    it->second.lru_iter = lru_order_.emplace(new_time, key);

    return true;
}

bool MermaidFileCache::LookupDimensions(uint64_t key, CacheEntry& entry) const noexcept
{
    key = InternalKey(key);
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;
    return true;
}

void MermaidFileCache::StoreAsync(uint64_t key, float css_width, float css_height, std::vector<uint8_t> png_data)
{
    if (png_data.empty()) {
        return;
    }
    key = InternalKey(key);

    const uint32_t png_size = static_cast<uint32_t>(png_data.size());

    EvictIfNeeded(png_size);

    auto& entry = index_[key];
    if (entry.png_size > 0) {
        DecrementTotalSize(entry.png_size);
        lru_order_.erase(entry.lru_iter);
    }
    entry.css_width = css_width;
    entry.css_height = css_height;
    entry.png_size = png_size;
    entry.last_used = Now();
    total_size_ += png_size;
    entry.lru_iter = lru_order_.emplace(entry.last_used, key);

    if (!scheduler_) {
        return;
    }

    // 書き込み開始前に in-flight セットへ登録しておく。
    // Lookup がファイル未着地を stale 扱いで index から消すのを防ぐ。
    {
        std::lock_guard lock(pending_mutex_);
        pending_writes_.insert(key);
    }

    const uint32_t gen = write_gen_.load();
    auto path = GetPngPath(key);
    scheduler_->Post([this, key, path = std::move(path), data = std::move(png_data), gen] {
        // タスク完遂・キャンセルどちらの場合も pending を必ず解除する
        struct PendingGuard {
            MermaidFileCache* self;
            uint64_t key;
            ~PendingGuard()
            {
                std::lock_guard lock(self->pending_mutex_);
                self->pending_writes_.erase(key);
            }
        } guard{ this, key };

        if (write_gen_.load() != gen) {
            return;
        }

        if (path.empty()) {
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        // WriteAllBytes 直前でも generation を再確認する。
        // ClearAll() / Shutdown() 後にディスクへ PNG を "復活" させて
        // しまう窓を最小化するための最後のガード。
        if (write_gen_.load() != gen) {
            return;
        }

        WriteAllBytes(path, data.data(), data.size());
    });
}

void MermaidFileCache::EvictIfNeeded(uint32_t new_png_size)
{
    const auto dir = GetCacheDir();

    while ((index_.size() >= max_entries_ || total_size_ + new_png_size > max_total_size_) && !lru_order_.empty()) {
        // LRU: multimapの先頭が最も古いエントリ（O(1)）
        const auto oldest_lru = lru_order_.begin();
        const uint64_t evict_key = oldest_lru->second;
        lru_order_.erase(oldest_lru);

        const auto it = index_.find(evict_key);
        if (it == index_.end()) {
            continue;
        }

        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::remove(GetPngPath(dir, evict_key), ec);
        }

        DecrementTotalSize(it->second.png_size);
        index_.erase(it);
    }
}

void MermaidFileCache::DecrementTotalSize(uint32_t png_size) noexcept
{
    if (total_size_ >= png_size) {
        total_size_ -= png_size;
    }
    else {
        total_size_ = 0;
    }
}

void MermaidFileCache::ClearAll()
{
    write_gen_.fetch_add(1);

    const auto dir = GetCacheDir();
    if (!dir.empty()) {
        std::error_code ec;
        for (const auto& [key, _] : index_) {
            std::filesystem::remove(GetPngPath(dir, key), ec);
        }
        std::filesystem::remove(dir / L"index.bin", ec);
    }

    index_.clear();
    lru_order_.clear();
    total_size_ = 0;
}

void MermaidFileCache::Shutdown()
{
    write_gen_.fetch_add(1);
    scheduler_ = nullptr;
}
