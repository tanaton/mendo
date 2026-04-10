#pragma once
#include <list>
#include <unordered_map>
#include <cstddef>

// 固定サイズの LRU (Least Recently Used) キャッシュ。
// Find() でアクセスするとアクセス順が更新される。
// 容量超過時は最も古いエントリが自動的に破棄される。
// O(1) の検索・挿入・削除。
// スレッド安全ではない。const Find() も内部の順序を変更するため、
// 複数スレッドからの同時アクセスには外部で排他制御が必要。
template <typename Key, typename Value>
class LruCache {
public:
    explicit LruCache(size_t max_entries) : max_entries_(max_entries) {}

    // キーに対応する値を検索し、見つかった場合はアクセス順を更新する。
    Value* Find(const Key& key)
    {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        // 先頭に移動（最近使用）
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->value;
    }

    const Value* Find(const Key& key) const
    {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        // const版でもアクセス順は更新する（mutableなorder_）
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->value;
    }

    // キーが存在するかチェック（アクセス順は更新しない）。
    bool Contains(const Key& key) const
    {
        return map_.find(key) != map_.end();
    }

    // エントリを挿入する。既存キーの場合は値を上書きしてアクセス順を更新する。
    // 容量超過時は最も古いエントリを自動的に破棄する。
    void Insert(const Key& key, Value value)
    {
        if (max_entries_ == 0) {
            return;
        }

        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->value = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }

        // 容量超過時は最古エントリを破棄
        if (map_.size() >= max_entries_) {
            auto& oldest = order_.back();
            map_.erase(oldest.key);
            order_.pop_back();
        }

        order_.push_front({ key, std::move(value) });
        map_.emplace(key, order_.begin());
    }

    void Clear()
    {
        order_.clear();
        map_.clear();
    }

    size_t Size() const noexcept { return map_.size(); }
    bool Empty() const noexcept { return map_.empty(); }
    size_t MaxSize() const noexcept { return max_entries_; }

private:
    struct Entry {
        Key key;
        Value value;
    };

    mutable std::list<Entry> order_;   // front=最新, back=最古
    // splice はイテレータを無効化しないため、map_ のイテレータは常に有効
    std::unordered_map<Key, typename std::list<Entry>::iterator> map_;
    size_t max_entries_;
};
