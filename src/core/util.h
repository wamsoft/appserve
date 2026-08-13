//---------------------------------------------------------------------------
// 内部ユーティリティ (公開ヘッダには出さない)
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace appserve {
namespace util {

// --- 文字列 ---
std::string toLower(std::string s);
std::string trim(const std::string& s);
bool        startsWith(const std::string& s, const std::string& prefix);
bool        endsWith(const std::string& s, const std::string& suffix);
/// 空要素を含めて分割する
std::vector<std::string> split(const std::string& s, char sep);
/// 先頭のトークンを取り出し、残りを rest へ (空白区切り)
std::string nextToken(const std::string& s, std::string& rest);

// --- パス ---
/// 実行ファイルのフルパス (UTF-8)。取得できなければ空。
std::string executablePath();
/// 実行ファイルのあるディレクトリ (末尾セパレータ無し)
std::string executableDir();
/// カレントディレクトリ (UTF-8)
std::string currentDir();
/// "/" 区切りに正規化 (Windows の "\" を "/" にする)
std::string normalizeSlash(std::string p);
/// dir と rel を連結する
std::string joinPath(const std::string& dir, const std::string& rel);
bool        isDirectory(const std::string& path);
bool        isFile(const std::string& path);
/// ファイルを丸ごと読む。失敗したら false。
bool        readFile(const std::string& path, std::string& out, uint64_t maxSize = 0);
bool        writeFile(const std::string& path, const std::string& data);

// --- 時刻 ---
/// 単調増加ミリ秒
int64_t nowMs();
/// "2026-08-14 12:34:56" 形式のローカル時刻
std::string timestamp();

// --- 乱数 ---
/// 暗号用途ではないが推測困難な hex 文字列 (nbytes*2 文字)
std::string randomHex(size_t nbytes);

} // namespace util
} // namespace appserve
