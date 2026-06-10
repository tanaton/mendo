#pragma once
// command_generator の分割ファイル間でのみ使用する内部共有宣言。

#ifdef MENDO_USE_TRACY

// command_generator.cpp で定義される Tracy プロット用統計カウンタ。
// 分割 cpp から MENDO_COUNT_INC/SET で参照するために外部リンケージが必要。
struct CmdGenStats {
    int64_t hittest_range = 0;
    int64_t sel_hl_cache_hit = 0;
    int64_t sel_hl_cache_miss = 0;
    int64_t search_hl_rebuild = 0;
    int64_t search_hl_provisional = 0;
    int64_t last_visible_node_count = 0;
};
extern CmdGenStats g_cmd_gen_stats;

#endif
