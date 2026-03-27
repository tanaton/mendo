#pragma once

// std::visit 用のオーバーロードヘルパー
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
