# mendo パフォーマンスレビュー

レビュー対象: `src/core/`, `src/layout/`, `src/render/`, `src/app/`, `src/io/`, `src/util/`, `src/input/` 全般

凡例: `[x]` 対応済み / `[~]` 部分対応 / `[ ]` 未着手 / `[-]` 全 revert / `(指示外)` 今回のスコープ外

---

## 対応状況サマリー

| 項目 | 状態 | 概要 |
|---|---|---|
| 1.1 md4c UTF-8 化 | `[x]` | type alias 抽象化 (`doc_char` / `doc_string` + `WideViewForDWrite` 境界) で C1〜C7 を達成。100MB 実機で Parse 560ms (UTF-16 同等) かつメモリ -100MB |
| 1.2 Node::text view 化 | `[x]` | view+owned 二経路化、view 化率 64.6%、22000 ノード × 数 KB 確保削減 |
| 1.3 MonotonicResource upstream | `[x]` | upstream を `new_delete_resource` に固定 |
| 1.4 ParseContext スクラッチ動的化 | `[x]` | `markdown_text.size()/8` を `[1KB, 64KB]` でクランプ |
| 1.5 OnText source_offset 分岐 | (指示外) | |
| 1.6 Alert prefix 合成 | `[ ]` | 1.1 と一括着手予定だったが 1.1 revert で凍結 |
| 2.1 不可視テーブル行 Evict | `[x]` | `EvictInvisibleTableRows` + `RestoreNullCellLayouts` lazy 復元 |
| 2.2 Y 計算 O(log n) 化 | `[x]` | `FloatFenwick` + `block_heights_`、`text_top` フィールドに統一 (Phase C 完了) |
| 2.3 幅バケット高さキャッシュ | `[x]` | `cached_width/height` で partial mode 不可視ノード回避 |
| 2.4 ズーム時 IDWriteTextLayout 保持 | (指示外) | |
| 2.5 NodeTableData::col_count | `[x]` | parser で確定、MeasureTable の `ranges::max` 排除 |
| 2.6 cell_layouts resize 改善 | `[x]` | `reserve + emplace_back` ループに置換 |
| 3.1 / 4.4 CommandList ノードキャッシュ | `[ ]` | S 級。試作で描画崩れ全 revert 済み (代替案は本文参照) |
| 3.2 DrawCommand に BrushId | `[x]` | `FixedBrushArray` 経由で O(1) ルックアップ |
| 3.3 SoA コマンドバッファ | `[x]` | `DrawCommandList` を SoA + tag/index sequence に再設計 |
| 3.4 テーブルセル選択キャッシュ | (指示外) | |
| 3.5 同色トークンマージ | `[x]` | `ApplyNodeEffects` で連結マージ |
| 3.6 ApplyTableEffects skip | (指示外) | |
| 4.1 MeasureNode 並列化 | `[x]` | `parallel_measure.h/.cpp`、4× 高速化 (W=4, N≥32 で実測) |
| 4.2 増分パース | `[ ]` | 超大規模、reload 経路の根本変更 |
| 4.3 テーブル SoA 化 | `[x]` | `NodeTableData` を SoA に再設計 (concat_text + offset 配列) |
| 4.5 thread-local pool 分離 | `[~]` | Renderer 5 メンバ + 局所 1 適用済。`MakePmrUnique` 経路は別作業 |
| 4.6 DPI 変更時 layout 保持 | `[x]` | `NotifyDpiChanged` で layout_dirty を立てない |
| 4.7 LayoutEngine 3 層分離 | `[x]` | `IMeasureBackend` / `LayoutComputer` / `DirtyScheduler` に分離 |
| 5系 (細かい改善) | `[~]` | バグ可能性確認済 (修正不要)、その他は指示外 |

---

## 全体所感

すでに「描画ホットパスの monotonic 化」「SBO TextRun」「行単位ビューポートカリング」「IDWriteTextLayout の SetMaxWidth ファストパス」「累積 X/Y 配列によるテーブルヒットテスト二分探索」「flat な inline-code 背景配列」「effects_generation キャッシュ」など、定番の最適化は徹底的に入っている。

残課題は (A) 巨大ファイル / (B) 巨大テーブル / (C) マルチコア活用 / (D) スクロール時の毎フレーム再生成 の 4 領域。

---

## 1. パーサー層

### 1.1 md4c の UTF-8 化 `[x]`

**ステータス**: type alias 抽象化方式で達成 → UTF-8 単一経路に整理完了。`MENDO_DOC_USE_UTF16` CMake オプションは廃止し、md4c は UTF-8 ビルド (`MD_CHAR=char`) 固定、Document 経路全体が UTF-8 byte ベース。

**実機計測 (100MB ファイル)**: Parse 560ms (旧 UTF-16 ビルド 600ms 比 -7%) / メモリ -100MB。Phase A 単独失敗時の 1.5× メモリプレッシャ + 3-4× 後退は完全に回避できた。

**経緯**: 「中間 commit は全て UTF-16 動作維持 / 最終切替は CMake オプション 1 行」を実現する type alias 層 (`mendo::doc_char` / `doc_string` / `MENDO_LIT()`) と DirectWrite 境界ラッパー `WideViewForDWrite` を導入し段階的に UTF-8 化を達成。両ビルド対応の検証完了後、UTF-16 分岐を全削除して UTF-8 単一経路に整理。

**主要 commit**:
- 88a02fd: alias 抽象化 (UTF-16 動作維持)
- e1c9f99: core/tests を UTF-8 切替に対応 (#if MENDO_DOC_USE_UTF16 で UTF-16/UTF-8 両ビルドの期待値を分岐)
- 38254d7: UTF-8 ビルドのシンタックスハイライト崩れ修正 (signed char で c>=0x80 が常に false / WideViewForDWrite::EnsureOffsetMap が未実装放置 / DirectWrite range の wide 変換漏れ)
- (UTF-8 標準化) CMake オプション削除、`#if MENDO_DOC_USE_UTF16` 全除去、`raw_wide_` → `raw_text_` rename

**主要ファイル**:
- `src/util/doc_text.h` (UTF-8 専用 alias、`MENDO_LIT(s)` は no-op マクロとして残置)
- `src/layout/doc_dwrite_bridge.{h,cpp}` (DirectWrite 用 UTF-16 view を per-call 構築)
- `src/core/document.{h,cpp}` (`raw_text_: mendo::doc_string`、`FromMarkdown(doc_string)`)
- `src/core/parser.{h,cpp}` (md4c コールバック、AppendDoc、ResolveHtmlEntity の UTF-8 4-byte 符号化)
- `src/core/document_utils.cpp` (GenerateAnchorIdInto の UTF-8 multi-byte decode 経路)
- `src/render/renderer.cpp` / `src/layout/dwrite_measurer.cpp` (run / token / alert range を `WideViewForDWrite::WideRange` 経由で UTF-16 textPosition に変換)
- `tests/` 全般 (UTF-8 byte 期待値、2143/2143 pass)

### 1.2 `Node::text` を `Document::raw_text_` への view 化 `[x]`

**完了**: Node 構造を view モード (`view_length: uint32_t` + `view_base_: const doc_char*`) と owned モード (`pmr_unique_ptr<pmr::string>`) の二経路に変更。`GetText() -> doc_string_view` で統一 API、view 化率 64.6% 維持 (test.md, CodeBlock 91121 chars view 化)。22000 ノード × 平均テキスト長分の `pmr::string` 確保が削減。Node sizeof: -4B/Node → 22000 ノードで -88KB。

**主要ファイル**: `src/core/document_types.h`, `src/core/document.cpp` (InjectViewBase + NormalizeNewlines), `src/core/parser.cpp` (memcmp ベースの view 化判定)。

**TableCell::text は scope 外** (raw_text_ の連続範囲ではないため `pmr::string` のまま)。

### 1.3 グローバル `synchronized_pool_resource` のロック競合 `[x]`

**完了**: `MonotonicResource` の upstream を `std::pmr::new_delete_resource()` に固定。parser/render の monotonic overflow 経路が sync pool を経由しなくなった。

**主要ファイル**: `src/util/memory_resource.h`。

### 1.4 `ParseContext` のスクラッチサイズ動的化 `[x]`

**完了**: `markdown_text.size() / 8` を `[1KB, 64KB]` でクランプする動的ヒント (`SCRATCH_RESERVE_MIN` / `SCRATCH_RESERVE_MAX`) に変更。

### 1.6 Alert prefix + raw_view 合成 `[ ]`

**ステータス**: 1.1 (UTF-8 化) は達成済みなので前提解消。ただし 1.2 の owned 経路で現状動作しており、UTF-8 化単独ではメモリ・速度のクリティカル経路ではない (100MB 計測で Parse 560ms 達成済み)。次回スコープに繰り延べ。

**残りの作業**: Alert ノードの先頭 prefix (アイコン + space + ラベル) を分離し、本文部分は `Document::raw_*_` への view にする。現状の owned コピーを削減し、view 化率を更に向上させる。

---

## 2. レイアウト / DirectWrite 計測層

### 2.1 巨大テーブル不可視セルの `CreateTextLayout` 削減 `[x]`

**完了**: `LayoutCache::EvictInvisibleTableRows` + `DWriteTextMeasurer::RestoreNullCellLayouts` で行単位 lazy 復元。`MeasureTable` の has_existing_layouts ブランチで `RestoreNullCellLayouts` を呼ぶ。`ResourceManager::EvictOffscreenBitmaps` から呼び出し。

**効果**: 5000 セル × 3KB = 15MB の常駐シェイピング結果を可視範囲のみに制限。

### 2.2 `RecomputeYPositions` の O(log n) 化 `[x]`

**完了**: `src/util/fenwick.h` (`FloatFenwick`) 新規 + `LayoutCache::block_heights_` 統合。Phase A〜C 全完了で `entry.y_position` フィールドを廃止し `text_top` (Fenwick の派生キャッシュ) に統一。WRITE 経路で Fenwick と同時更新するため二重 SSOT 描画崩れを構造的に解消。

`EstimateNodeHeights` / `LayoutEngine::ComputeLayout` (broke_early=false 経路) の `BuildBlockHeights` バルクロード化で O(N log N) → O(N) に削減 (N=22000 で ~28× 高速化)。

**主要ファイル**: `src/util/fenwick.h`, `src/layout/layout_cache.h`, `src/layout/layout_computer.h/.cpp`, `src/layout/layout.cpp`。

### 2.3 partial mode 不可視ノードの `EstimateNodeHeight` `[x]`

**完了**: `NodeLayoutEntry` に `cached_width` / `cached_height` 追加。MeasureNode 後に書き、partial mode の不可視ノードでは `cached_width` 一致 (epsilon=0.5px) なら `cached_height` をフォールバック使用。

### 2.5 `NodeTableData::col_count` 保持 `[x]`

**完了**: `col_count` (uint16_t) を parser の TR/TH/TD で確定。`MeasureTable` の全行 `ranges::max` 走査を排除。

### 2.6 `cell_layouts.resize` の zero-init 削減 `[x]`

**完了**: `clear()` + `reserve()` + N 回 `emplace_back()` ループに置換。

---

## 3. レンダラ / コマンド層

### 3.1 / 4.4 `ID2D1CommandList` ノードキャッシュ `[ ]`

**ステータス**: S 級。前セッションで PR1+PR2+PR3 を試作したが、実機 UI で「スクロール/クリックの瞬間だけ表示、他は真っ白」の描画崩れが再現し全 revert。

**失敗の主因 (推定)**: `DrawImage(ID2D1CommandList)` の二重 transform 適用や CL の dc binding 仕様の壁。WIC bitmap RT 経由の単体テスト (2137 件) では QI 失敗で no-op に倒れるため検出不能。

**再挑戦時の代替案**:
1. **per-node `DrawCommandList` (C++ 構造体) のフレーム間キャッシュ**: ID2D1CommandList を使わず、`DrawCommandList` (SoA) 自体を entry に持たせる。スクロール時は `CommandExecutor::Execute` で再生するだけ。CommandGenerator の発行コストは消える。CL の transform 仕様問題を回避できる
2. **完全オフスクリーン RT + DrawBitmap**: 各ノードを `ID2D1Bitmap1` にラスタライズ、`DrawBitmap` 一発。VRAM 増 + ClearType 劣化 (grayscale AA) を許容できれば最強
3. **ID2D1CommandList 再挑戦**: `IDXGISwapChain1` の swap effect / `ID2D1DeviceContext::SetTarget` の正確な仕様 / `ID2D1Image::GetImageLocalBounds` を Tracy で計測しながら進める

### 3.2 CommandExecutor の brush LRU 改善 `[x]`

**完了**: `BrushId` enum を `src/render/brush_id.h` に切り出し、各 DrawCommand 構造体に `brush_id` フィールド追加。`CommandExecutor::FixedBrushArray` 経由で id != Custom かつ配列ヒット時は O(1) ルックアップ。

### 3.3 DrawCommand SoA 化 `[x]`

**完了**: `DrawCommandList` を class 化し、各 Cmd 型ごとの `pmr::vector` + `pmr::vector<uint32_t> seq_` (tag/index pack) に分割。`CommandExecutor::Execute` 専用に `Visit` テンプレートで switch 駆動の走査を提供 (variant + visit のオーバーヘッド削除)。

**互換**: `DrawCommand` (`std::variant`) は temp として残置 (テスト用 `At()` / `front` / `back` 経路でのみ使用)。

### 3.5 同色トークンマージ `[x]`

**完了**: `ApplyNodeEffects` 内で同 type の隣接トークン (`pending_end == token.start`) を 1 つの range にまとめてから `SetDrawingEffect` を発行。

---

## 4. アーキテクチャレベルの改善案

### 4.1 `MeasureNode` per-node 並列化 `[x]`

**完了**: `src/layout/parallel_measure.h/.cpp` 新設。`mendo::layout::RunParallel` を `LayoutEngine::ProcessDirtyBatch` で `layout_scheduler != nullptr` なら呼び分け。`kMinDirtyForParallel=32` 未満は inline 直列、以上で `chunk_size = clamp(N/(W*4), 16, 512)` で動的分割し `std::latch` で待機。Post 失敗時は UI スレッド fallback。

CodeBlock の Tokenize 結果は per-slot vector を `indices.size()` で事前確保し in-order 集約 (sort 不要)。Table の `linearized_text` を `TableLayoutData` に移し Node 副作用排除。

**ベンチ実測** (Mock + 100µs busy-spin、W=4): N=32 から 2.0× (32 / 64 で約 4×)、N=22000 で 3.9× (2.20s → 564ms)。

**主要ファイル**: `src/layout/parallel_measure.h/.cpp`, `src/layout/measure_backend.h` (`tokens_out` 追加), `src/layout/dwrite_measurer.cpp`, `src/app/app.h` (`layout_scheduler_` 分離)。

### 4.2 増分パース `[ ]`

**ステータス**: 超大規模、未着手。`ReplaceFromMarkdown` での全パースを、変更ブロックだけ再パース + LayoutCache の prefix 保持に変える設計。reload diff の根本変更が必要。

### 4.3 テーブルデータの SoA 再設計 `[x]`

**完了**: `NodeTableData` を SoA (`concat_text` + `cell_text_starts` + `cell_run_starts` + `all_runs` + `aligns` + `is_header_row` + `row_count` + `col_count`) に再設計。`TableCell` / `TableRow` / `Node::table_rows()` / `BuildLinearizedTableText` / `AdvanceFlatOffsetInRow` 等を完全撤去。

**新 API**: `tbl->GetCellText(r,c)` / `GetCellRuns(r,c)` / `IsHeaderRow(r)` / `ColAlign(c)`。

### 4.5 thread-local pool 分離 `[~]`

**部分完了**: `src/util/memory_resource.h::GetThreadLocalPoolResource()` 追加 (`thread_local std::pmr::unsynchronized_pool_resource`、upstream は `new_delete_resource`)。

**適用済み**:
- Renderer の `hit_test_buffer_` / `cached_toast_text_` / `cached_search_text_` / `cached_search_query_` / `cached_search_ime_comp_`
- `renderer_search.cpp::DrawSearchBar` 内の局所 `display_buf`

**未適用**: `SearchHlCache` 等の `MakePmrUnique<T>()` 経由は `pmr_unique_ptr` の deleter 拡張 (resource 保持型) が必要で別作業。+8B/ノード × 22000 = +176KB の trade-off あり。

### 4.6 DPI 変更時の layout 保持 `[x]`

**完了**: `LayoutCache::NotifyDpiChanged` 新設。`per_frame_hl_caches` invalidate と `effects_generation_` インクリメントのみで、`text_layout` / `layout_dirty` / `first_line_height` を触らない。22000 ノード規模で数百 ms かかっていた DPI 変更時再レイアウトをほぼ 0 に。

### 4.7 LayoutEngine の 3 層分離 `[x]`

**完了**: `IMeasureBackend` (per-node 計測) / `IMeasureLifecycle` (Init/Recreate) を `src/layout/measure_backend.h` に分離。`mendo::layout::*` (`layout_computer.h/.cpp`) で自由関数群を namespace 集約。`mendo::layout::DirtyScheduler::RunSerial` (`dirty_scheduler.h/.cpp`) で選定+予算管理を分離。

`ITextMeasurer` は 2 IF 継承の空合成 class として残置で既存 60+ テスト/呼び出し元ノータッチ。`MeasureNode`/`MeasureTable` を const 化 (4.1 並列化の前提)。

**残課題**: `EnsureVisibleLayout` の `DirtyScheduler::RunVisibleOnly` 統合 (今回 scope 外)。

---

## 5. 細かい改善

| 場所 | 内容 | ステータス |
|---|---|---|
| `parser.cpp:832` `nodes.reserve` 上限 | 上限を 16384 → 262144 に引き上げ。100MB スケールで realloc 14回→4回に削減 | `[x]` |
| `parser.cpp:218` 1 文字 chunk fastpath | direct write の余地 | `[ ]` |
| `dwrite_measurer.cpp:472` `ranges::max` | parser で col_count 確定 | `[x]` (2.5 と統合) |
| `command_generator.cpp:541` `equal_range` | `lower_bound` 2 回より早いか実測要 | `[ ]` |
| `selection_html.cpp:548` `out.reserve(estimated * 3)` | `* 4` にする余地 | `[ ]` |
| `layout.cpp:62` `fold_left` → `std::reduce` | SIMD 化 | `[x]` |
| `command_executor.cpp:43` 経路 | rehash 安全性 | `[x]` バグなし確認 |
| `renderer.cpp:194` `hit_test_buffer_` | 二重持ち削減 | `[x]` |

---

## 6. プロファイル/計測の補強

`profiler.h::MENDO_PROFILE` インフラがあるので、以下を計測してから優先順位を決めると良い:

1. 典型ファイル (1MB / 10MB / 巨大表 / 巨大コードブロック) での Parse / 全 MeasureNode / GenerateMdPane / CommandExecutor::Execute
2. リサイズドラッグ中のフレームタイム
3. スクロール中 (60Hz の 1 フレーム内訳)
4. テーマ切替時の合計時間

---

## 7. 優先度サマリー (ROI 順)

| 優先度 | 項目 | ステータス |
|---|---|---|
| ★★★ | 4.1 MeasureNode per-node 並列化 | `[x]` |
| ★★★ | 4.4 / 3.1 ID2D1CommandList ノードキャッシュ | `[ ]` 試作 revert 済 |
| ★★ | 2.1 巨大テーブル不可視セル削減 | `[x]` |
| ★★ | 2.2 prefix-sum tree Y 計算 | `[x]` |
| ★★ | 1.2 Node::text view 化 | `[x]` |
| ★★ | 1.1 md4c UTF-8 化 | `[x]` UTF-8 単一経路に整理完了 |
| ★ | 4.2 増分パース | `[ ]` |
| ★ | 4.3 テーブル SoA 化 | `[x]` |
| ★ | 3.2 / 3.3 brush map / variant 改善 | `[x]` |
| ★ | 4.5 thread-local pool 分離 | `[~]` |

---

## 8. 次セッションでの再開メモ

### 残課題 (優先度順)

1. **3.1 / 4.4 CommandList ノードキャッシュ (★★★)**: 試作で描画崩れ revert 済。代替案 1 (per-node `DrawCommandList` C++ 構造体キャッシュ) が最も現実的
2. **4.2 増分パース (★)**: 超大規模、reload 経路の根本変更
3. **4.5 拡張**: `SearchHlCache` 等の `MakePmrUnique` 経路を thread-local pool に乗せる (`pmr_unique_ptr` の deleter 拡張が必要)
4. **5系の小改善**: parser.cpp の 1 文字 chunk fastpath / equal_range 検討等
5. **BuildHeadingIndices 40ms 改善**: § 9 で見送り。`anchor_index_` の `try_emplace` で wstring 都度生成。C++23 に transparent オーバーロードがないため、別アプローチ (anchor 文字列の concat buffer + wstring_view キー化等) が必要

### 既知の制約

- `Node::view_base_` は `const mendo::doc_char*` (UTF-8 統一後は `const char*`)、`Document::raw_text_` を維持必須 (relocate 禁止)
- `entry.y_position` は廃止済、`entry.text_top` (Fenwick 派生キャッシュ) に統一
- `LayoutEngine` は `IMeasureBackend` + `LayoutComputer` + `DirtyScheduler` の 3 層分離済み (4.1 並列化の前提)

---

## 9. 局所最適化 (md4c UTF-16 維持セッション)

旧「UTF-8 化を諦め、`raw_wide_` の I/O 経路最適化など局所改善で済ませる」方針で実施したセッションの記録 (md4c は UTF-16 ビルド維持時)。本項以降の言及はすべて履歴であり、現在は UTF-8 単一経路に整理済み。

### 実機タイミング (100MB ファイル)

- FileLoader::LoadFile: 120ms (memmap + MultiByteToWideChar)
- NormalizeNewlines: 30ms
- ParseMarkdown: 600ms (md4c 本体 300ms + callback 300ms)
- BuildHeadingIndices: 40ms

合計約 790ms。dominant は ParseMarkdown (76%)。

### 実施 Phase

| Phase | 内容 | ステータス |
|---|---|---|
| 0 | `MENDO_STATF` を parser hotspot に追加。`MatchRawSlice` の `MENDO_PROFILE` は高頻度ゾーン (40万回超) で zone overhead が大きいため後で削除 | `[x]` |
| A | `nodes.reserve` 上限 16384 → 262144、`list_counter.reserve(8)` 追加 | `[x]` |
| B | `CurrentTextMatchesRawSlice` に prefix/suffix probe (kProbe=256 wchar) 導入。512 wchar 未満は変更なし | `[x]` |
| C-1 | `parse_resource` 初期サイズを `clamp(input/20, 128KB, 5MB)` で動的化 | `[x]` |
| C-2 | NormalizeNewlines AVX2 化 | `[-]` 見送り (実測 3.8%、閾値 5% 未満) |
| C-3 | BuildHeadingIndices の wstring 都度生成排除 | `[-]` 見送り (C++23 標準に `try_emplace` の transparent オーバーロードなし。設計変更大の代替案は別セッション扱い) |
| E | owned 確定経路のショートサーキット (`current_node_owned_only` フラグ)。OnEnterSpan / MD_TEXT_ENTITY/BR/SOFTBR で立て、FinalizeCurrentNode で memcmp スキップ | `[x]` |
| F | BuildHeadingIndices の最適化試行 (試案 1: `std::map<std::wstring, ...>` / 試案 2: `std::unordered_map<std::wstring, ...>` + `emplace(piecewise_construct, ...)`) | `[-]` 両案とも revert。試案 1 は 40 → 60ms 悪化 (tree node 逐次 alloc が `reserve(N)` 済 hash table に劣る)。試案 2 は 40ms 不変で改善なし、pmr::wstring 維持の memory resource 統一管理を優先して revert |
| G | callback 内 hot path の `active_text_buffer` ポインタキャッシュ化。`AppendWide` / `FlushPendingRun` での `in_table_cell + has_table()` 判定 (47-94万回) を排除 | `[x]` |

### 主要修正ファイル
- `src/core/parser.cpp` (Phase 0/A/B/C-1/E/G)

### 検証結果
- ビルド: `cmake --build build --config Release` 成功
- 既存テスト: `mendo_tests.exe` で 2143 件 pass
- view 化率: test.md 64.645% / nested.md 73.5294% で変更前後不変 → Phase B の probe memcmp 誤判定なし
- 大型 view 化ノード (516〜3592 wchar) で `first_diff == text.size()` 確認 (probe 経路で完全一致を保証)

### 効果計測 (Tracy、100MB ファイル、ノード数 468000)

ユーザー Tracy 計測の実測値:

| 段階 | ParseMarkdown(md_parse) | 累計改善 |
|---|---|---|
| 改善前 (推定) | ~600ms | - |
| Phase A+B+C-1 後 | 570ms | -30ms / -5% |
| Phase 0 の `MatchRawSlice` MENDO_PROFILE 削除後 | 550ms | -50ms |
| Phase E (owned ショートサーキット) 後 | 540ms | -60ms / 約 -10% |
| Phase F 試案 1 (BuildHeadingIndices map 化) | revert | 40ms → 60ms に悪化したため見送り |
| Phase G (active_text_buffer キャッシュ) 後 | 550ms | 誤差レベル (Tracy 単発計測) |
| Phase F 試案 2 (emplace + piecewise_construct + std::wstring キー) | revert | 40ms 不変。pmr::wstring の memory resource 統一管理を優先 |

**最終結果**: ParseMarkdown 600ms → 550ms (約 -8%)、BuildHeadingIndices 40ms 維持。

ParseMarkdown 全体としては 600 → 550ms (約 -8%) で打ち止め。さらに削るには callback ごとのベンチマークプログラムで誤差排除した計測か、本質的設計変更 (md4c UTF-8 化 / 増分パース等) が必要。

学び:
- 高頻度ゾーン (40万回/parse) に `MENDO_PROFILE` を入れると zone overhead で計測自体が歪む。Tracy ON 時のみ約 20ms のオーバーヘッドだった (本来コード自体には no-op 影響だが、Tracy ON ビルドでの計測誤差として現れる)
- Phase B の probe (kProbe=256) は平均ノード長 110 wchar (= 100MB / 47万ノード ÷ 2 byte) では発動条件 (≥ 512 wchar) を満たさず、ほぼ no-op だった可能性。huge_codeblock のような大型単一ノードを持つファイルでのみ意味あり
- 大半の改善源は Phase A (reserve 上限引き上げ) + Phase C-1 (parse_resource 動的化)
- Phase E (owned ショートサーキット) は owned 経路 35% の memcmp 完全スキップで 10ms 削減 (見積もり 8-16ms の範囲内)
- 残る 540ms のうち md4c 本体 ~300ms は不変、callback 内 ~240ms (BeginNode / AppendWide / FlushPendingRun 等) が次セッションの改善余地

各 Phase は単独で revert 可能。本セッションで Phase 0 の MatchRawSlice 計測ゾーンは効果なしと判明し削除済。Phase B は 100MB の通常ファイルでは実効性が limited だが、構造的に worst case bounded で副作用なし、特殊ケースで効果あるため維持。
