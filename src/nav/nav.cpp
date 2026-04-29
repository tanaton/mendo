#include "nav.h"
#include "ascii_util.h"

uint32_t NavHistory::InternPath(std::wstring_view path)
{
    if (last_interned_index_ != std::numeric_limits<uint32_t>::max() && last_interned_view_ == path) {
        return last_interned_index_;
    }
    if (const auto it = path_index_.find(path); it != path_index_.end()) {
        last_interned_view_ = it->first;
        last_interned_index_ = it->second;
        return it->second;
    }

    uint32_t idx;
    if (!free_slots_.empty()) {
        // 解放済みスロットを再利用してプールサイズの単調増加を防ぐ。
        idx = free_slots_.back();
        free_slots_.pop_back();
        path_pool_[idx].text.assign(path);
        path_pool_[idx].refcount = 0;
    }
    else {
        idx = static_cast<uint32_t>(path_pool_.size());
        path_pool_.emplace_back(PathSlot{ std::pmr::wstring(path), 0u });
    }
    auto& slot = path_pool_[idx];
    const std::wstring_view view = slot.text;
    path_index_.emplace(view, idx);
    last_interned_view_ = view;
    last_interned_index_ = idx;
    return idx;
}

void NavHistory::RetainPath(uint32_t idx) noexcept
{
    if (idx < path_pool_.size()) {
        ++path_pool_[idx].refcount;
    }
}

void NavHistory::ReleasePath(uint32_t idx) noexcept
{
    if (idx >= path_pool_.size()) {
        return;
    }
    auto& slot = path_pool_[idx];
    if (slot.refcount == 0) {
        return;
    }
    if (--slot.refcount == 0) {
        // text.clear() で view が無効化されるため、先に index を消す
        path_index_.erase(std::wstring_view(slot.text));
        if (last_interned_index_ == idx) {
            last_interned_view_ = {};
            last_interned_index_ = std::numeric_limits<uint32_t>::max();
        }
        // text の capacity はあえて保持する。次に free_slots_ から
        // 取り出された際、同程度の長さのパスで assign が再割り当てせずに済む。
        slot.text.clear();
        free_slots_.push_back(idx);
    }
}

NavHistory::InternalEntry NavHistory::ToInternal(const NavEntry& e)
{
    const uint32_t idx = InternPath(e.file_path);
    RetainPath(idx);
    return { idx, e.node, e.offset };
}

NavEntry NavHistory::ToExternal(const InternalEntry& e) const
{
    return NavEntry(path_pool_[e.path_index].text, e.node, e.offset);
}

void NavHistory::Push(const NavEntry& current)
{
    back_stack_.emplace_back(ToInternal(current));
    // 新規ナビゲーションでは進むスタックを破棄する。各エントリの参照を解放する
    for (const auto& fe : forward_stack_) {
        ReleasePath(fe.path_index);
    }
    forward_stack_.clear();

    // 履歴サイズを制限（dequeなのでpop_frontはO(1)）
    if (back_stack_.size() > MAX_HISTORY) {
        ReleasePath(back_stack_.front().path_index);
        back_stack_.pop_front();
    }
}

bool NavHistory::GoBack(const NavEntry& current, NavEntry& out)
{
    if (back_stack_.empty()) {
        return false;
    }

    forward_stack_.emplace_back(ToInternal(current));
    if (forward_stack_.size() > MAX_HISTORY) {
        ReleasePath(forward_stack_.front().path_index);
        forward_stack_.pop_front();
    }
    const auto top = back_stack_.back();
    out = ToExternal(top);
    back_stack_.pop_back();
    ReleasePath(top.path_index);
    return true;
}

bool NavHistory::GoForward(const NavEntry& current, NavEntry& out)
{
    if (forward_stack_.empty()) {
        return false;
    }

    back_stack_.emplace_back(ToInternal(current));
    const auto top = forward_stack_.back();
    out = ToExternal(top);
    forward_stack_.pop_back();
    ReleasePath(top.path_index);
    return true;
}

void NavHistory::Clear() noexcept
{
    back_stack_.clear();
    forward_stack_.clear();
    path_pool_.clear();
    path_index_.clear();
    free_slots_.clear();
    last_interned_view_ = {};
    last_interned_index_ = std::numeric_limits<uint32_t>::max();
}

// ShellExecuteWに渡しても安全なURLスキームかどうかを判定する。
// file:// やその他の危険なスキームをブロックし、http/https/mailto のみ許可する。
bool IsSafeUrlScheme(std::wstring_view url) noexcept
{
    return ascii_util::istarts_with(url, L"http://")
        || ascii_util::istarts_with(url, L"https://")
        || ascii_util::istarts_with(url, L"mailto:");
}

LinkClickResult HandleLinkClick(std::wstring_view url)
{
    LinkClickResult result;
    if (url.empty()) {
        return result;
    }
    // 内部アンカーリンク: #something
    if (url[0] == L'#') {
        result.type = LinkClickResult::Type::Anchor;
        result.target = url.substr(1);
        return result;
    }
    // 安全なスキームの外部リンクのみ許可
    if (!IsSafeUrlScheme(url)) {
        return result;
    }
    result.type = LinkClickResult::Type::ExternalUrl;
    result.target = url;
    return result;
}
