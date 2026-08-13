//---------------------------------------------------------------------------
// ログ
//---------------------------------------------------------------------------
#include "appserve/log.h"
#include "core/util.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>

namespace appserve {

namespace {

std::atomic<int>  g_level{(int)LogLevel::Info};
std::atomic<bool> g_console{true};

std::mutex g_mu;                                   // sink 表 + ファイル出力の保護
std::vector<std::pair<int, LogSink>> g_sinks;
int          g_next_handle = 1;
std::ofstream g_file;

} // anonymous

//---------------------------------------------------------------------------
void setLogLevel(LogLevel lv) { g_level.store((int)lv, std::memory_order_relaxed); }
LogLevel logLevel() { return (LogLevel)g_level.load(std::memory_order_relaxed); }

const char* logLevelName(LogLevel lv)
{
	switch (lv) {
		case LogLevel::Verbose: return "verbose";
		case LogLevel::Debug:   return "debug";
		case LogLevel::Info:    return "info";
		case LogLevel::Warn:    return "warn";
		case LogLevel::Error:   return "error";
		case LogLevel::Off:     return "off";
	}
	return "info";
}

bool parseLogLevel(const std::string& s, LogLevel& out)
{
	std::string v = util::toLower(util::trim(s));
	if (v == "verbose" || v == "trace") { out = LogLevel::Verbose; return true; }
	if (v == "debug")                   { out = LogLevel::Debug;   return true; }
	if (v == "info")                    { out = LogLevel::Info;    return true; }
	if (v == "warn" || v == "warning")  { out = LogLevel::Warn;    return true; }
	if (v == "error")                   { out = LogLevel::Error;   return true; }
	if (v == "off" || v == "none")      { out = LogLevel::Off;     return true; }
	return false;
}

//---------------------------------------------------------------------------
void log(LogLevel lv, const std::string& line)
{
	if ((int)lv < g_level.load(std::memory_order_relaxed)) return;

	// sink 呼び出し中に addLogSink されるとデッドロックするので、
	// ロック下でコピーを取ってからロック外で呼ぶ。
	std::vector<std::pair<int, LogSink>> sinks;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		sinks = g_sinks;
		if (g_file.is_open()) {
			g_file << util::timestamp() << " [" << logLevelName(lv) << "] "
			       << line << "\n";
			g_file.flush();
		}
	}

	if (g_console.load(std::memory_order_relaxed)) {
		const char* tag = logLevelName(lv);
		std::fprintf(stderr, "[%s] %s\n", tag, line.c_str());
		std::fflush(stderr);
	}

	for (auto& s : sinks) {
		if (s.second) s.second(lv, line);
	}
}

//---------------------------------------------------------------------------
int addLogSink(LogSink sink)
{
	std::lock_guard<std::mutex> lk(g_mu);
	int h = g_next_handle++;
	g_sinks.emplace_back(h, std::move(sink));
	return h;
}

void removeLogSink(int handle)
{
	std::lock_guard<std::mutex> lk(g_mu);
	for (size_t i = 0; i < g_sinks.size(); ++i) {
		if (g_sinks[i].first == handle) { g_sinks.erase(g_sinks.begin() + (long)i); return; }
	}
}

bool setLogFile(const std::string& path)
{
	std::lock_guard<std::mutex> lk(g_mu);
	if (g_file.is_open()) g_file.close();
	if (path.empty()) return true;
	g_file.open(path, std::ios::app);
	return g_file.is_open();
}

void setConsoleLogging(bool enable)
{
	g_console.store(enable, std::memory_order_relaxed);
}

} // namespace appserve
