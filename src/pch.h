#pragma once

// <windows.h> 取り込みの前提:
// UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN は target_compile_definitions で定義済み

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
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <windows.h>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>

// <rpcndr.h> の `#define small char` を全 TU に伝播させない
#ifdef small
#  undef small
#endif
