#include "mermaid_file_cache.h"
#include "config_store.h"
#include <fstream>
#include <algorithm>
#include <chrono>

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
    auto base = config::GetConfigDir();
    if (base.empty()) {
        return {};
    }
    return base / L"MermaidCache";
}

std::filesystem::path MermaidFileCache::GetPngPath(uint64_t key) const
{
    auto dir = GetCacheDir();
    if (dir.empty()) {
        return {};
    }
    wchar_t name[24];
    swprintf_s(name, L"%016llx.png", key);
    return dir / name;
}

std::filesystem::path MermaidFileCache::GetIndexPath() const
{
    auto dir = GetCacheDir();
    if (dir.empty()) {
        return {};
    }
    return dir / L"index.bin";
}

int64_t MermaidFileCache::Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void MermaidFileCache::Init(float current_dpr)
{
    current_dpr_ = current_dpr;
    auto dir = GetCacheDir();
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

    // バックグラウンドライタースレッドを起動
    shutdown_flag_.store(false);
    writer_thread_ = std::thread(&MermaidFileCache::WriterLoop, this);
}

void MermaidFileCache::LoadIndex()
{
    index_.clear();
    total_size_ = 0;

    auto path = GetIndexPath();
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

        index_[key] = entry;
        total_size_ += entry.png_size;
    }
}

void MermaidFileCache::SaveIndex()
{
    auto path = GetIndexPath();
    if (path.empty()) {
        return;
    }

    auto dir = GetCacheDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return;
    }

    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t count = static_cast<uint32_t>(index_.size());

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

    auto path = GetPngPath(key);
    if (path.empty()) {
        return false;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        // ファイルが存在しない（書き込み前 or 削除済み）→ 古いインデックスエントリを除去
        if (total_size_ >= it->second.png_size) {
            total_size_ -= it->second.png_size;
        } else {
            total_size_ = 0;
        }
        index_.erase(it);
        return false;
    }

    auto size = ifs.tellg();
    if (size <= 0) {
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

    // last_usedを更新
    it->second.last_used = Now();

    return true;
}

void MermaidFileCache::StoreAsync(uint64_t key, float css_width, float css_height,
    std::vector<uint8_t> png_data)
{
    if (png_data.empty()) {
        return;
    }

    uint32_t png_size = static_cast<uint32_t>(png_data.size());

    // 必要に応じてLRU削除
    EvictIfNeeded(png_size);

    // インデックスエントリを即座に追加・更新
    auto& entry = index_[key];
    if (entry.png_size > 0 && total_size_ >= entry.png_size) {
        total_size_ -= entry.png_size;
    }
    entry.css_width = css_width;
    entry.css_height = css_height;
    entry.png_size = png_size;
    entry.last_used = Now();
    total_size_ += png_size;

    // バックグラウンドスレッドに書き出しを依頼
    {
        std::lock_guard lock(writer_mutex_);
        write_queue_.push({ key, std::move(png_data) });
    }
    writer_cv_.notify_one();
}

void MermaidFileCache::EvictIfNeeded(uint32_t new_png_size)
{
    while ((index_.size() >= max_entries_ ||
        total_size_ + new_png_size > max_total_size_) &&
        !index_.empty()) {
        // LRU: last_usedが最も古いエントリを探す
        auto oldest = index_.begin();
        for (auto it = index_.begin(); it != index_.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used) {
                oldest = it;
            }
        }

        // PNGファイルを削除
        auto path = GetPngPath(oldest->first);
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        if (total_size_ >= oldest->second.png_size) {
            total_size_ -= oldest->second.png_size;
        } else {
            total_size_ = 0;
        }
        index_.erase(oldest);
    }
}

void MermaidFileCache::ClearAll()
{
    // 保留中の書き込みを破棄
    {
        std::lock_guard lock(writer_mutex_);
        std::queue<WriteRequest> empty;
        write_queue_.swap(empty);
    }

    // すべてのPNGファイルとインデックスファイルを削除
    auto dir = GetCacheDir();
    if (!dir.empty()) {
        std::error_code ec;
        for (const auto& [key, _] : index_) {
            std::filesystem::remove(GetPngPath(key), ec);
        }
        std::filesystem::remove(GetIndexPath(), ec);
    }

    index_.clear();
    total_size_ = 0;
}

void MermaidFileCache::Shutdown()
{
    if (writer_thread_.joinable()) {
        shutdown_flag_.store(true);
        writer_cv_.notify_one();
        writer_thread_.join();
    }
}

void MermaidFileCache::WriterLoop()
{
    while (true) {
        WriteRequest req;
        {
            std::unique_lock lock(writer_mutex_);
            writer_cv_.wait(lock, [this] {
                return !write_queue_.empty() || shutdown_flag_.load();
            });
            if (write_queue_.empty()) {
                if (shutdown_flag_.load()) {
                    return;
                }
                continue;
            }
            req = std::move(write_queue_.front());
            write_queue_.pop();
        }

        // PNGファイルを書き出す
        auto path = GetPngPath(req.key);
        if (path.empty()) {
            continue;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::ofstream ofs(path, std::ios::binary);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(req.png_data.data()),
                static_cast<std::streamsize>(req.png_data.size()));
        }
    }
}
