#pragma once

namespace mendo {

// グローバル名前空間汚染を避けるため mendo:: 配下に置く。
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

} // namespace mendo
