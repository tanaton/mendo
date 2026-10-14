#pragma once
#include <cstdint>

// マウスジェスチャー / 水平スワイプが示す履歴移動の向き。
enum class NavDirection : uint8_t {
    None,
    Back,
    Forward
};
