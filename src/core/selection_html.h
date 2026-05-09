#pragma once
#include "document_types.h"
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

// 選択範囲に基づいてノードから選択テキストを抽出する。
// ノード間を \r\n で連結したテキストを返す。
std::pmr::string ExtractSelectedText(const std::pmr::vector<Node>& nodes, const TextSelection& selection);

// 選択範囲を HTML フラグメントに変換する。
// 見出し/段落/リスト/引用/コードブロック/テーブル/インライン強調/リンクを HTML タグへ変換する。
// dark_mode 時はコードブロック背景とシンタックスハイライトの色、テーブル border 色が
// ダークテーマ相当（VS Code Dark+ 風）に切り替わる。貼付け先の背景色に合わせてユーザーが
// テーマを切り替えて使う想定。
// 戻り値は CF_HTML のフラグメント部（<!--StartFragment--> と <!--EndFragment--> の間に入る本文）。
std::pmr::string ExtractSelectedTextAsHtml(
    const std::pmr::vector<Node>& nodes,
    const TextSelection& selection,
    bool dark_mode = false);

// 単一の CodeBlock ノード全体をシンタックスハイライト付きの HTML フラグメント (UTF-8) に変換する。
// コードブロック右上のコピーボタンから書式付きでクリップボードへ書き込むために使用する。
// node が CodeBlock 以外の場合は空文字列を返す。
std::pmr::string BuildCodeBlockHtmlFragment(const Node& node, bool dark_mode);

// ノードのラン内の指定テキスト位置にあるリンクURLを検索する。
// リンクラン内の位置であればリンクURLを返し、そうでなければnulloptを返す。
[[nodiscard]] std::optional<std::pmr::string> FindLinkAtPosition(const Node& node, uint32_t text_pos);
