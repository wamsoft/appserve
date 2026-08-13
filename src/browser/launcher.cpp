//---------------------------------------------------------------------------
// ブラウザ起動 実装
//---------------------------------------------------------------------------
#include "browser/launcher.h"
#include "appserve/log.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <cstring>
extern char** environ;
#endif

namespace appserve {
namespace browser {

namespace {

std::string joinArgs(const std::vector<std::string>& args)
{
	std::string s;
	for (const auto& a : args) {
		if (!s.empty()) s += ' ';
		s += a;
	}
	return s;
}

#ifdef _WIN32
std::wstring toW(const std::string& utf8)
{
	if (utf8.empty()) return std::wstring();
	int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
	if (len <= 0) return std::wstring();
	std::wstring out((size_t)len, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &out[0], len);
	return out;
}

/// exe を引数付きで起動する。App Paths が効くので "msedge.exe" のような
/// 名前だけでフルパス解決される (インストールされていなければ失敗する)。
bool execProgram(const std::string& exe, const std::string& args)
{
	std::wstring wexe  = toW(exe);
	std::wstring wargs = toW(args);
	HINSTANCE r = ::ShellExecuteW(nullptr, L"open", wexe.c_str(),
	                              wargs.empty() ? nullptr : wargs.c_str(),
	                              nullptr, SW_SHOWNORMAL);
	// ShellExecute の戻り値は「32 より大きければ成功」という古い規約
	return (INT_PTR)r > 32;
}
#else
/// argv[0] を PATH から探して起動する (親は待たない)
bool execProgram(const std::vector<std::string>& argv)
{
	if (argv.empty()) return false;
	std::vector<char*> cargv;
	cargv.reserve(argv.size() + 1);
	for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
	cargv.push_back(nullptr);

	pid_t pid = 0;
	if (posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), environ) != 0)
		return false;

	// 起動直後に失敗していないかを軽く確認する (すぐ終了 = 起動できなかった)
	int status = 0;
	for (int i = 0; i < 20; ++i) {
		pid_t r = ::waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			if (WIFEXITED(status) && WEXITSTATUS(status) != 0) return false;
			return true;   // 正常終了 (別プロセスへ委譲するブラウザもある)
		}
		if (r < 0) return true;   // 追跡できない = 起動はした
		::usleep(25 * 1000);
	}
	return true;   // まだ生きている = 起動成功
}
#endif

} // anonymous

//---------------------------------------------------------------------------
bool launchAppMode(const std::string& url, const std::vector<std::string>& extraArgs)
{
	const std::string appArg = "--app=" + url;
	const std::string extra  = joinArgs(extraArgs);

#ifdef _WIN32
	std::string args = appArg;
	if (!extra.empty()) args += " " + extra;
	// Edge → Chrome の順 (Windows では Edge が必ず入っている)
	if (execProgram("msedge.exe", args)) return true;
	if (execProgram("chrome.exe", args)) return true;
	return false;
#elif defined(__APPLE__)
	// open -n -a <App> --args --app=<url>
	static const char* kApps[] = {
		"Microsoft Edge", "Google Chrome", "Chromium", nullptr
	};
	for (int i = 0; kApps[i]; ++i) {
		std::vector<std::string> argv = { "open", "-n", "-a", kApps[i], "--args", appArg };
		for (const auto& a : extraArgs) argv.push_back(a);
		if (execProgram(argv)) return true;
	}
	return false;
#else
	static const char* kBins[] = {
		"microsoft-edge", "google-chrome", "google-chrome-stable",
		"chromium", "chromium-browser", nullptr
	};
	for (int i = 0; kBins[i]; ++i) {
		std::vector<std::string> argv = { kBins[i], appArg };
		for (const auto& a : extraArgs) argv.push_back(a);
		if (execProgram(argv)) return true;
	}
	return false;
#endif
}

//---------------------------------------------------------------------------
bool openDefault(const std::string& url)
{
#ifdef _WIN32
	std::wstring wurl = toW(url);
	HINSTANCE r = ::ShellExecuteW(nullptr, L"open", wurl.c_str(),
	                              nullptr, nullptr, SW_SHOWNORMAL);
	return (INT_PTR)r > 32;
#elif defined(__APPLE__)
	return execProgram({ "open", url });
#else
	return execProgram({ "xdg-open", url });
#endif
}

//---------------------------------------------------------------------------
bool open(const std::string& url, bool appMode, const std::vector<std::string>& extraArgs)
{
	if (url.empty()) return false;
	if (appMode) {
		if (launchAppMode(url, extraArgs)) {
			logI("browser: opened in app mode");
			return true;
		}
		logD("browser: app mode unavailable, falling back to the default browser");
	}
	if (openDefault(url)) {
		logI("browser: opened in the default browser");
		return true;
	}
	logW("browser: failed to open " + url);
	return false;
}

} // namespace browser
} // namespace appserve
