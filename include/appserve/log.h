//---------------------------------------------------------------------------
// ログ
//
// レベル付き 1 行ログ + sink。sink はサーバ稼働中に SSE (/_app/events) へ
// 流すために使われる。sink は任意スレッドから呼ばれるので実装側で排他すること。
//---------------------------------------------------------------------------
#pragma once
#include <functional>
#include <string>

namespace appserve {

enum class LogLevel { Verbose = 0, Debug, Info, Warn, Error, Off };

/// 表示レベル。これ未満は捨てられる (既定 Info)
void     setLogLevel(LogLevel lv);
LogLevel logLevel();
/// "verbose" / "debug" / "info" / "warn" / "error" / "off" を解釈
bool     parseLogLevel(const std::string& s, LogLevel& out);
const char* logLevelName(LogLevel lv);

/// ログ 1 行を出力する。改行不要。
void log(LogLevel lv, const std::string& line);

inline void logV(const std::string& s) { log(LogLevel::Verbose, s); }
inline void logD(const std::string& s) { log(LogLevel::Debug,   s); }
inline void logI(const std::string& s) { log(LogLevel::Info,    s); }
inline void logW(const std::string& s) { log(LogLevel::Warn,    s); }
inline void logE(const std::string& s) { log(LogLevel::Error,   s); }

/// 追加 sink。戻り値は解除用のハンドル。任意スレッドから呼ばれる。
using LogSink = std::function<void(LogLevel, const std::string&)>;
int  addLogSink(LogSink sink);
void removeLogSink(int handle);

/// ログをファイルにも書く (追記)。空文字で無効化。
bool setLogFile(const std::string& path);

/// 標準エラー出力への書き出しを止める / 再開する
void setConsoleLogging(bool enable);

} // namespace appserve
