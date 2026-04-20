#pragma once
#include "timer_ids.h"
#include "app_messages.h"
#include "search_bar_controller.h"
#include "resource_manager.h"
#include "mermaid.h"

// タイマーIDはtimer_ids.hで定義。コンポーネント定数との一致をコンパイル時検証する。
static_assert(app_timer::SEARCH_CARET == SearchBarController::TIMER_CARET);
static_assert(app_timer::SEARCH_DEBOUNCE == SearchBarController::TIMER_DEBOUNCE);
static_assert(app_timer::MERMAID_BATCH == ResourceManager::TIMER_MERMAID_BATCH);
static_assert(app_timer::BITMAP_MANAGE == ResourceManager::TIMER_BITMAP_MANAGE);
static_assert(app_timer::MERMAID_INIT_RETRY == MermaidRenderer::TIMER_INIT_RETRY);
