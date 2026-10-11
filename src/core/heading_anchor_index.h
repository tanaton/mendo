#pragma once
#include "string_hash.h"
#include <cstddef>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>

// 見出しのアンカー (GitHub と同じスラグ) と、それを付けた見出しのノード番号の索引。
// 重複は github-slugger と同じく出現順に "-1", "-2", ... を付けるので、見出しは文書順に Add する。
class HeadingAnchorIndex {
public:
    // 次の見出しを登録し、割り当てたアンカーを返す。返り値は索引を破棄するまで有効。
    std::string_view Add(std::string_view heading_text, int node_index);

    // anchor (ASCII 小文字) を付けた見出しのノード番号。登録済みの見出しに無ければ -1。
    int Find(std::string_view anchor) const;

    // 登録済みの見出し数。続きから登録するときの開始位置になる。
    size_t heading_count() const noexcept
    {
        return heading_count_;
    }

    // 合計 n 件の見出しを登録するまで再ハッシュしないようにする。
    void Reserve(size_t n)
    {
        entries_.reserve(n);
    }

private:
    struct Entry {
        int node_index = -1;
        // このスラグを素のスラグとする見出しに付けた連番の最大値。
        int counter = 0;
    };

    // 要素は索引ごと破棄するまで消さないので、キー文字列と要素を 1 件ずつヒープに確保せず arena に積む。
    std::pmr::monotonic_buffer_resource arena_;
    // 採用した連番付きスラグも登録し、同じ文字列の見出しが後に来たら衝突として扱う。
    std::pmr::unordered_map<std::pmr::string, Entry, StringTransparentHash, std::equal_to<>> entries_{ &arena_ };
    size_t heading_count_ = 0;
    std::pmr::string base_;
    std::pmr::string candidate_;
};
