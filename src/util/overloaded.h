#pragma once

namespace mendo {

// std::visit 用のオーバーロードヘルパー。グローバル名前空間汚染を避けるため mendo:: 配下に置く。
// テンプレート使用箇所だけが include すればよく、std/Win32 ヘッダに依存しないので
// include 連鎖を浅く保てる。
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

} // namespace mendo
