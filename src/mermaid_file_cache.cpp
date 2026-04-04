#include "mermaid_file_cache.h"
#include "task_scheduler.h"
#include "config_store.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cmath>

MermaidFileCache::~MermaidFileCache()
{
    Shutdown();
}

void MermaidFileCache::SetCacheDir(const std::filesystem::path& dir)
{
    cache_dir_override_ = dir;
}

void MermaidFileCache::SetLimits(size_t max_entries, uint64_t max_total_size)
{
    max_entries_ = max_entries;
    max_total_size_ = max_total_size;
}

std::filesystem::path MermaidFileCache::GetCacheDir() const
{
    if (!cache_dir_override_.empty()) {
        return cache_dir_override_;
    }
    const auto base = config::GetConfigDir();
    if (base.empty()) {
        return {};
    }
    return base / L"MermaidCache";
}

std::filesystem::path MermaidFileCache::GetPngPath(uint64_t key) const
{
    const auto dir = GetCacheDir();
    if (dir.empty()) {
        return {};
    }
    wchar_t name[24];
    swprintf_s(name, L"%016llx.png", key);
    return dir / name;
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
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void MermaidFileCache::RemoveLruEntry(int64_t timestamp, uint64_t key)
{
    const auto [lo, hi] = lru_order_.equal_range(timestamp);
    for (auto iter = lo; iter != hi; ++iter) {
        if (iter->second == key) {
            lru_order_.erase(iter);
            return;
        }
    }
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

    // 保存済みDPRと現在のDPRが異なる場合、全キャッシュを削除
    if (stored_dpr_ != 0.0f && stored_dpr_ != current_dpr) {
        ClearAll();
    }
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

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return;
    }

    // ヘッダー読み込み
    uint32_t magic = 0, version = 0, count = 0;
    float dpr = 0.0f;
    ifs.read(reinterpret_cast<char*>(&magic), 4);
    ifs.read(reinterpret_cast<char*>(&version), 4);
    ifs.read(reinterpret_cast<char*>(&dpr), 4);
    ifs.read(reinterpret_cast<char*>(&count), 4);

    if (!ifs || magic != kMagic || version != kVersion) {
        return;
    }
    // 異常なエントリ数を拒否
    if (count > kDefaultMaxEntries * 2) {
        return;
    }

    stored_dpr_ = dpr;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t key = 0;
        IndexEntry entry;
        ifs.read(reinterpret_cast<char*>(&key), 8);
        ifs.read(reinterpret_cast<char*>(&entry.css_width), 4);
        ifs.read(reinterpret_cast<char*>(&entry.css_height), 4);
        ifs.read(reinterpret_cast<char*>(&entry.png_size), 4);
        ifs.read(reinterpret_cast<char*>(&entry.last_used), 8);
        if (!ifs) {
            break;
        }

        // 壊れたエントリを無視する
        if (entry.css_width <= 0.0f || entry.css_height <= 0.0f ||
            !std::isfinite(entry.css_width) || !std::isfinite(entry.css_height) ||
            entry.png_size == 0) {
            continue;
        }

        index_[key] = entry;
        lru_order_.emplace(entry.last_used, key);
        total_size_ += entry.png_size;
    }
}

void MermaidFileCache::SaveIndex()
{
    const auto path = GetIndexPath();
    if (path.empty()) {
        return;
    }

    const auto dir = GetCacheDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return;
    }

    const uint32_t magic = kMagic;
    const uint32_t version = kVersion;
    const uint32_t count = static_cast<uint32_t>(index_.size());

    ofs.write(reinterpret_cast<const char*>(&magic), 4);
    ofs.write(reinterpret_cast<const char*>(&version), 4);
    ofs.write(reinterpret_cast<const char*>(&current_dpr_), 4);
    ofs.write(reinterpret_cast<const char*>(&count), 4);

    for (const auto& [key, entry] : index_) {
        ofs.write(reinterpret_cast<const char*>(&key), 8);
        ofs.write(reinterpret_cast<const char*>(&entry.css_width), 4);
        ofs.write(reinterpret_cast<const char*>(&entry.css_height), 4);
        ofs.write(reinterpret_cast<const char*>(&entry.png_size), 4);
        ofs.write(reinterpret_cast<const char*>(&entry.last_used), 8);
    }
}

bool MermaidFileCache::Lookup(uint64_t key, CacheEntry& entry, std::vector<uint8_t>& png_data)
{
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }

    const auto path = GetPngPath(key);
    if (path.empty()) {
        return false;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        // ファイルが存在しない（書き込み前 or 削除済み）→ 古いインデックスエントリを除去
        if (total_size_ >= it->second.png_size) {
            total_size_ -= it->second.png_size;
        }
        else {
            total_size_ = 0;
        }
        RemoveLruEntry(it->second.last_used, key);
        index_.erase(it);
        return false;
    }

    const auto size = ifs.tellg();
    if (size <= 0) {
        RemoveLruEntry(it->second.last_used, key);
        index_.erase(it);
        return false;
    }

    ifs.seekg(0);
    png_data.resize(static_cast<size_t>(size));
    ifs.read(reinterpret_cast<char*>(png_data.data()), size);
    if (!ifs) {
        return false;
    }

    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;

    // last_usedを更新し、LRU順序を再配置
    const int64_t old_time = it->second.last_used;
    const int64_t new_time = Now();
    it->second.last_used = new_time;
    RemoveLruEntry(old_time, key);
    lru_order_.emplace(new_time, key);

    return true;
}

bool MermaidFileCache::LookupDimensions(uint64_t key, CacheEntry& entry) const noexcept
{
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;
    return true;
}

void MermaidFileCache::StoreAsync(uint64_t key, float css_width, float css_height,
    std::vector<uint8_t> png_data)
{
    if (png_data.empty()) {
        return;
    }

    const uint32_t png_size = static_cast<uint32_t>(png_data.size());

    // 必要に応じてLRU削除
    EvictIfNeeded(png_size);

    // インデックスエントリを即座に追加・更新
    auto& entry = index_[key];
    if (entry.png_size > 0 && total_size_ >= entry.png_size) {
        total_size_ -= entry.png_size;
        RemoveLruEntry(entry.last_used, key);
    }
    entry.css_width = css_width;
    entry.css_height = css_height;
    entry.png_size = png_size;
    entry.last_used = Now();
    total_size_ += png_size;
    lru_order_.emplace(entry.last_used, key);

    if (!scheduler_) {
        return;
    }

    // バックグラウンドスレッドに書き出しを依頼
    const uint32_t gen = write_gen_.load();
    auto path = GetPngPath(key);
    scheduler_->Post([this, path = std::move(path), data = std::move(png_data), gen] {
        if (write_gen_.load() != gen) {
            return;
        }

        if (path.empty()) {
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::ofstream ofs(path, std::ios::binary);
        if (ofs) {
            ofs.write(
                reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size())
            );
        }
    });
}

void MermaidFileCache::EvictIfNeeded(uint32_t new_png_size)
{
    while ((index_.size() >= max_entries_ ||
        total_size_ + new_png_size > max_total_size_) &&
        !lru_order_.empty()) {
        // LRU: multimapの先頭が最も古いエントリ（O(1)）
        const auto oldest_lru = lru_order_.begin();
        const uint64_t evict_key = oldest_lru->second;
        lru_order_.erase(oldest_lru);

        const auto it = index_.find(evict_key);
        if (it == index_.end()) {
            continue;
        }

        // PNGファイルを削除
        const auto path = GetPngPath(evict_key);
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        if (total_size_ >= it->second.png_size) {
            total_size_ -= it->second.png_size;
        }
        else {
            total_size_ = 0;
        }
        index_.erase(it);
    }
}

void MermaidFileCache::ClearAll()
{
    // 保留中の書き込みタスクを無効化
    write_gen_.fetch_add(1);

    // すべてのPNGファイルとインデックスファイルを削除
    const auto dir = GetCacheDir();
    if (!dir.empty()) {
        std::error_code ec;
        for (const auto& [key, _] : index_) {
            std::filesystem::remove(GetPngPath(key), ec);
        }
        std::filesystem::remove(GetIndexPath(), ec);
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
