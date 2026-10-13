#pragma once
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

// エンコード済み PNG。図のビットマップ・ディスクキャッシュ・クリップボードで共有する。
using PngBytes = std::shared_ptr<const std::pmr::vector<uint8_t>>;
