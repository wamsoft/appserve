//---------------------------------------------------------------------------
// コマンドライン解析
//---------------------------------------------------------------------------
#include "core/app_impl.h"
#include "appserve/log.h"
#include "core/util.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>          // CommandLineToArgvW
#endif

namespace appserve {

namespace {

#ifdef _WIN32
/// Windows の argv は UTF-8 ではなく ANSI (日本語環境なら CP932) で渡ってくる。
/// そのまま扱うと「D:/絵/画面.psd」のようなパスが開けないので、UTF-16 の
/// コマンドラインから取り直して UTF-8 へ直す。取れなければ false (元の argv を使う)。
bool wideArgs(std::vector<std::string>& out)
{
	int n = 0;
	LPWSTR* w = ::CommandLineToArgvW(::GetCommandLineW(), &n);
	if (!w) return false;
	out.clear();
	for (int i = 0; i < n; ++i) {
		int len = ::WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
		if (len <= 1) { out.push_back(std::string()); continue; }
		std::string s((size_t)len - 1, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
		out.push_back(s);
	}
	::LocalFree(w);
	return true;
}
#endif

/// "HOST:PORT" / "PORT" を解釈する (吉里吉里Z の -replweb と同じ書式)
bool parseListen(const std::string& v, std::string& host, int& port)
{
	size_t colon = v.rfind(':');
	if (colon == std::string::npos) {
		port = atoi(v.c_str());
		return port >= 0 && port < 65536;
	}
	std::string h = v.substr(0, colon);
	if (!h.empty()) host = h;
	port = atoi(v.c_str() + colon + 1);
	return port >= 0 && port < 65536;
}

void printHelp(const Options& o, const std::vector<OptionSpec>& custom)
{
	std::printf(
		"%s %s\n"
		"\n"
		"Usage: %s [options] [args...]\n"
		"\n"
		"Network:\n"
		"  --host=ADDR           bind address (default 127.0.0.1; \"0.0.0.0\" for all)\n"
		"  --port=N              port (default 0 = let the OS pick a free one)\n"
		"  --listen=HOST:PORT    shorthand for the two above\n"
		"\n"
		"UI:\n"
		"  --web-root=PATH       UI directory or .zip to serve\n"
		"  --spa                 fall back to index.html for unknown paths\n"
		"\n"
		"Browser:\n"
		"  --browser=MODE        app | default | none   (default app)\n"
		"  --browser-arg=ARG     extra argument for the browser (repeatable)\n"
		"  --no-browser          same as --browser=none\n"
		"\n"
		"Lifecycle (seconds, 0 disables):\n"
		"  --idle-timeout=SEC    exit after no browser for SEC (default %d)\n"
		"  --startup-grace=SEC   grace period after start (default %d)\n"
		"  --session-ttl=SEC     session liveness timeout (default %d)\n"
		"\n"
		"REPL:\n"
		"  --repl                interactive REPL on stdin\n"
		"  --replfile=DIR        file-channel REPL (for agents / CI)\n"
		"\n"
		"Files:\n"
		"  --root=DIR            restrict file API to DIR (repeatable)\n"
		"  --allow-write         enable write/mkdir/remove APIs\n"
		"\n"
		"Security:\n"
		"  --token=STR           use a fixed token instead of a random one\n"
		"  --no-token            disable token checks (development only)\n"
		"\n"
		"Misc:\n"
		"  --plugin-dir=DIR      plugin search directory (default <exe>/plugins)\n"
		"  --no-plugins          do not load dynamic plugins\n"
		"  --log-level=LEVEL     verbose | debug | info | warn | error | off\n"
		"  --log-file=PATH       also append logs to PATH\n"
		"  --version             print version and exit\n"
		"  --help                print this help and exit\n",
		o.appName.c_str(), o.appVersion.c_str(), o.appName.c_str(),
		o.idleTimeout, o.startupGrace, o.sessionTtl);

	if (!custom.empty()) {
		std::printf("\n%s options:\n", o.appName.c_str());
		for (const auto& c : custom) {
			std::string left = "  --" + c.name;
			if (!c.valueHint.empty()) left += "=" + c.valueHint;
			while (left.size() < 24) left += ' ';
			std::printf("%s%s\n", left.c_str(), c.help.c_str());
		}
	}
}

} // anonymous

//---------------------------------------------------------------------------
bool App::parseArgs(int argc, char** argv)
{
	Options& o = opt_;
	exitCode_ = 0;

	// 引数は UTF-8 として扱う。Windows だけは argv が ANSI なので取り直す。
	std::vector<std::string> args;
	for (int i = 0; i < argc; ++i) args.push_back(argv[i] ? argv[i] : "");
#ifdef _WIN32
	std::vector<std::string> wide;
	if (wideArgs(wide) && wide.size() == args.size()) args.swap(wide);
#endif

	for (size_t i = 1; i < args.size(); ++i) {
		const std::string& a = args[i];
		if (a.empty()) continue;

		// オプション以外は位置引数として貯める
		if (a.size() < 2 || a[0] != '-') { o.args.push_back(a); continue; }

		// "--name=value" / "--name" に分解 ("-name" 形式も受ける)
		std::string body = (a.size() > 2 && a[1] == '-') ? a.substr(2) : a.substr(1);
		std::string name = body, value;
		bool hasValue = false;
		size_t eq = body.find('=');
		if (eq != std::string::npos) {
			name     = body.substr(0, eq);
			value    = body.substr(eq + 1);
			hasValue = true;
		}

		auto needValue = [&](const char* what) -> bool {
			if (hasValue) return true;
			std::fprintf(stderr, "error: --%s requires a value\n", what);
			exitCode_ = 2;
			return false;
		};

		if (name == "help" || name == "h") { printHelp(o, impl_->customOptions); return false; }
		if (name == "version") {
			std::printf("%s %s\n", o.appName.c_str(), o.appVersion.c_str());
			return false;
		}

		if (name == "host") {
			if (!needValue("host")) return false;
			o.host = value;
		} else if (name == "port") {
			if (!needValue("port")) return false;
			o.port = atoi(value.c_str());
			if (o.port < 0 || o.port >= 65536) {
				std::fprintf(stderr, "error: invalid port '%s'\n", value.c_str());
				exitCode_ = 2;
				return false;
			}
		} else if (name == "listen") {
			if (!needValue("listen")) return false;
			if (!parseListen(value, o.host, o.port)) {
				std::fprintf(stderr, "error: invalid --listen '%s'\n", value.c_str());
				exitCode_ = 2;
				return false;
			}
		} else if (name == "web-root" || name == "webroot") {
			if (!needValue("web-root")) return false;
			o.webRoot = value;
		} else if (name == "spa") {
			o.spa = true;
		} else if (name == "browser") {
			if (!needValue("browser")) return false;
			std::string v = util::toLower(value);
			if (v == "app")          o.browser = Options::BrowserMode::App;
			else if (v == "default") o.browser = Options::BrowserMode::Default;
			else if (v == "none")    o.browser = Options::BrowserMode::None;
			else {
				std::fprintf(stderr, "error: --browser must be app|default|none\n");
				exitCode_ = 2;
				return false;
			}
		} else if (name == "browser-arg") {
			if (!needValue("browser-arg")) return false;
			o.browserArgs.push_back(value);
		} else if (name == "no-browser") {
			o.browser = Options::BrowserMode::None;
		} else if (name == "idle-timeout") {
			if (!needValue("idle-timeout")) return false;
			o.idleTimeout = atoi(value.c_str());
		} else if (name == "startup-grace") {
			if (!needValue("startup-grace")) return false;
			o.startupGrace = atoi(value.c_str());
		} else if (name == "session-ttl") {
			if (!needValue("session-ttl")) return false;
			o.sessionTtl = atoi(value.c_str());
		} else if (name == "repl") {
			o.repl = !hasValue || (value != "0" && util::toLower(value) != "no" &&
			                       util::toLower(value) != "off");
		} else if (name == "replfile") {
			if (!needValue("replfile")) return false;
			o.replFile = value;
		} else if (name == "root") {
			if (!needValue("root")) return false;
			o.roots.push_back(util::normalizeSlash(value));
		} else if (name == "allow-write") {
			o.allowWrite = true;
		} else if (name == "token") {
			if (!needValue("token")) return false;
			o.token = value;
		} else if (name == "no-token") {
			o.useToken = false;
		} else if (name == "plugin-dir") {
			if (!needValue("plugin-dir")) return false;
			o.pluginDir = value;
		} else if (name == "no-plugins") {
			o.loadPlugins = false;
		} else if (name == "log-level") {
			if (!needValue("log-level")) return false;
			if (!parseLogLevel(value, o.logLevel)) {
				std::fprintf(stderr, "error: unknown log level '%s'\n", value.c_str());
				exitCode_ = 2;
				return false;
			}
		} else if (name == "log-file") {
			if (!needValue("log-file")) return false;
			o.logFile = value;
		} else {
			// 派生アプリの独自オプション
			bool handled = false;
			for (const auto& c : impl_->customOptions) {
				if (c.name != name) continue;
				handled = true;
				if (c.apply && !c.apply(value)) {
					std::fprintf(stderr, "error: invalid value for --%s\n", name.c_str());
					exitCode_ = 2;
					return false;
				}
				break;
			}
			if (!handled) {
				std::fprintf(stderr, "error: unknown option '%s' (try --help)\n", a.c_str());
				exitCode_ = 2;
				return false;
			}
		}
	}

	// トークンを明示指定したなら検証を有効にする
	if (!o.token.empty()) o.useToken = true;
	return true;
}

} // namespace appserve
