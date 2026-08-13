//---------------------------------------------------------------------------
// PluginLoader 実装
//---------------------------------------------------------------------------
#include "plugin/loader.h"
#include "plugin/registry.h"

#include "appserve/app.h"
#include "appserve/log.h"
#include "appserve/plugin_abi.h"
#include "core/util.h"

#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace appserve {

namespace {

//---------------------------------------------------------------------------
// C ABI ⇄ C++ の橋渡し。ホスト関数は ctx に Bridge* を入れて渡す。
struct Bridge {
	App*      app = nullptr;
	Registry* registry = nullptr;
	std::string pluginName;
};

AppserveStr mkStr(const std::string& s)
{
	AppserveStr r;
	r.ptr = s.data();
	r.len = s.size();
	return r;
}

std::string toStd(const AppserveStr& s)
{
	return (s.ptr && s.len) ? std::string(s.ptr, s.len) : std::string();
}

AppserveStr reqHeader(const AppserveReq* self, const char* key)
{
	AppserveStr none{nullptr, 0};
	if (!self || !self->impl || !key) return none;
	const Request* req = (const Request*)self->impl;
	// ヘッダ値の寿命を保つため Request 内の実体を指す
	std::string k = util::toLower(key);
	for (const auto& kv : req->headers.all()) {
		if (kv.first == k) return mkStr(kv.second);
	}
	return none;
}

/// AppserveResp を Response に写し取り、プラグインの解放関数を呼ぶ
Response takeResp(AppserveResp& r)
{
	Response out;
	out.status = r.status ? r.status : 200;
	out.mime   = r.mime.ptr ? toStd(r.mime) : std::string("application/json; charset=utf-8");
	out.body   = toStd(r.body);
	if (r.free_body && r.body.ptr) r.free_body((void*)r.body.ptr);
	return out;
}

//---------------------------------------------------------------------------
void host_route(void* ctx, const char* prefix, int affinity,
                AppserveHandlerFn fn, void* user)
{
	Bridge* b = (Bridge*)ctx;
	if (!b || !b->registry || !prefix || !fn) return;
	Affinity aff = (affinity == APPSERVE_AFFINITY_ANY) ? Affinity::Any : Affinity::Main;

	b->registry->route(prefix, aff, [fn, user](const Request& req) -> Response {
		AppserveReq creq;
		creq.method = mkStr(req.method);
		creq.path   = mkStr(req.path);
		creq.prefix = mkStr(req.prefix);
		creq.suffix = mkStr(req.suffix);
		creq.query  = mkStr(req.query);
		creq.body   = mkStr(req.body);
		creq.header = reqHeader;
		creq.impl   = (void*)&req;

		AppserveResp cresp;
		cresp.status    = 200;
		cresp.mime      = AppserveStr{nullptr, 0};
		cresp.body      = AppserveStr{nullptr, 0};
		cresp.free_body = nullptr;

		fn(&creq, &cresp, user);
		return takeResp(cresp);
	});
}

void host_repl_command(void* ctx, const char* name, const char* help,
                       AppserveReplFn fn, void* user)
{
	Bridge* b = (Bridge*)ctx;
	if (!b || !b->registry || !name || !fn) return;
	b->registry->replCommand(name, help ? help : "", [fn, user](const std::string& args) {
		AppserveResp cresp;
		cresp.status    = 200;
		cresp.mime      = AppserveStr{nullptr, 0};
		cresp.body      = AppserveStr{nullptr, 0};
		cresp.free_body = nullptr;
		fn(args.c_str(), &cresp, user);
		Response r = takeResp(cresp);
		return r.body;
	}, Affinity::Main);
}

void host_broadcast(void* ctx, const char* channel, const char* payload)
{
	Bridge* b = (Bridge*)ctx;
	if (!b || !b->app || !channel) return;
	b->app->browser().broadcast(channel, payload ? payload : "");
}

void host_log(void* ctx, int level, const char* message)
{
	Bridge* b = (Bridge*)ctx;
	if (!message) return;
	LogLevel lv = LogLevel::Info;
	switch (level) {
		case APPSERVE_LOG_VERBOSE: lv = LogLevel::Verbose; break;
		case APPSERVE_LOG_DEBUG:   lv = LogLevel::Debug;   break;
		case APPSERVE_LOG_WARN:    lv = LogLevel::Warn;    break;
		case APPSERVE_LOG_ERROR:   lv = LogLevel::Error;   break;
		default:                   lv = LogLevel::Info;    break;
	}
	log(lv, (b ? "[" + b->pluginName + "] " : std::string()) + message);
}

int host_port(void* ctx)
{
	Bridge* b = (Bridge*)ctx;
	return (b && b->app) ? b->app->port() : 0;
}

const char* host_app_name(void* ctx)
{
	Bridge* b = (Bridge*)ctx;
	return (b && b->app) ? b->app->options().appName.c_str() : "";
}

//---------------------------------------------------------------------------
void* openLibrary(const std::string& path)
{
#ifdef _WIN32
	int len = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
	std::wstring w((size_t)(len > 0 ? len : 0), L'\0');
	if (len > 0)
		::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &w[0], len);
	return (void*)::LoadLibraryW(w.c_str());
#else
	return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* findSymbol(void* handle, const char* name)
{
#ifdef _WIN32
	return (void*)::GetProcAddress((HMODULE)handle, name);
#else
	return ::dlsym(handle, name);
#endif
}

void closeLibrary(void* handle)
{
#ifdef _WIN32
	::FreeLibrary((HMODULE)handle);
#else
	::dlclose(handle);
#endif
}

const char* libExtension()
{
#ifdef _WIN32
	return ".dll";
#elif defined(__APPLE__)
	return ".dylib";
#else
	return ".so";
#endif
}

/// Bridge の寿命はプロセス終了までなので leak させて良い (プラグインが
/// ホスト関数を後から呼ぶ可能性があるため、unload まで生かす)
std::vector<std::unique_ptr<Bridge>> g_bridges;

} // anonymous

//---------------------------------------------------------------------------
PluginLoader::PluginLoader(App& app, Registry& registry)
	: app_(app), registry_(registry) {}

PluginLoader::~PluginLoader() { unloadAll(); }

//---------------------------------------------------------------------------
int PluginLoader::loadDirectory(const std::string& dir)
{
	if (dir.empty() || !util::isDirectory(dir)) {
		logD("plugin dir not present: " + dir);
		return 0;
	}
	int count = 0;
	std::error_code ec;
	for (auto it = fs::directory_iterator(fs::u8path(dir), ec);
	     it != fs::directory_iterator(); it.increment(ec)) {
		if (ec) break;
		if (!it->is_regular_file(ec)) continue;
		std::string path = util::normalizeSlash(it->path().u8string());
		if (!util::endsWith(util::toLower(path), libExtension())) continue;
		if (loadFile(path)) ++count;
	}
	if (count) logI("loaded " + std::to_string(count) + " plugin(s) from " + dir);
	return count;
}

//---------------------------------------------------------------------------
bool PluginLoader::loadFile(const std::string& path)
{
	void* handle = openLibrary(path);
	if (!handle) {
		logW("plugin load failed: " + path);
		return false;
	}
	auto init = (int (*)(const AppserveHost*))findSymbol(handle, "appserve_plugin_init");
	if (!init) {
		logW("plugin has no appserve_plugin_init: " + path);
		closeLibrary(handle);
		return false;
	}

	auto bridge = std::make_unique<Bridge>();
	bridge->app        = &app_;
	bridge->registry   = &registry_;
	{
		size_t sl = path.find_last_of('/');
		bridge->pluginName = (sl == std::string::npos) ? path : path.substr(sl + 1);
	}

	AppserveHost host;
	host.abi_version  = APPSERVE_ABI_VERSION;
	host.ctx          = bridge.get();
	host.route        = host_route;
	host.repl_command = host_repl_command;
	host.broadcast    = host_broadcast;
	host.log          = host_log;
	host.port         = host_port;
	host.app_name     = host_app_name;

	// 登録されるルートの owner をプラグイン名にする
	registry_.setOwner(bridge->pluginName);
	int ok = 0;
	try {
		ok = init(&host);
	} catch (...) {
		ok = 0;
	}
	registry_.setOwner("app");

	if (!ok) {
		logW("plugin init returned failure (abi mismatch?): " + path);
		closeLibrary(handle);
		return false;
	}

	g_bridges.push_back(std::move(bridge));
	loaded_.push_back(Loaded{path, handle});
	logI("plugin loaded: " + path);
	return true;
}

//---------------------------------------------------------------------------
void PluginLoader::unloadAll()
{
	// ルートの解除はホスト側 (Registry::clear) が先に行われている前提。
	// ここでハンドルを閉じないと、プラグイン内のコードを指す std::function が
	// 残っていた場合にクラッシュするので、閉じるのは最後にする。
	for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
		if (it->handle) closeLibrary(it->handle);
	}
	loaded_.clear();
	g_bridges.clear();
}

} // namespace appserve
