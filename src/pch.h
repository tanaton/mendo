#pragma once

// <windows.h> の前提の UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN は target_compile_definitions で定義済み。

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <utility>
#include <variant>
#include <vector>

#include <windows.h>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>

// windows.h 経由の <rpcndr.h> (MIDL ランタイム) は `#define small char` を出し、`small` を識別子に使う標準ライブラリや
// 自前コードをコンパイルエラーにする。pch から全 TU に広がらないようここで解除する (small を使わなくても保険として残す)。
#ifdef small
#undef small
#endif
