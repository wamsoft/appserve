//---------------------------------------------------------------------------
// App::Impl — アプリ内部状態 (公開ヘッダには出さない)
//---------------------------------------------------------------------------
#pragma once
#include "http/server.h"          // winsock2 を先に取り込む必要があるので先頭

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "appserve/app.h"
#include "core/session.h"
#include "core/taskqueue.h"
#include "plugin/registry.h"
#include "webroot/webroot.h"

namespace appserve {

class Repl;
class PluginLoader;

struct App::Impl {
	explicit Impl(App& owner);
	~Impl();

	App&           owner;
	TaskQueue      queue;
	SessionManager sessions;
	Registry       registry;
	BrowserChannel browser;
	HttpServer     server;
	WebRoot        webroot;

	std::vector<std::unique_ptr<IModule>> modules;
	std::vector<OptionSpec>               customOptions;
	std::unique_ptr<Repl>                 repl;
	std::unique_ptr<PluginLoader>         plugins;

	std::atomic<bool> running{false};
	std::atomic<int>  exitCode{0};
	int64_t           startMs = 0;
	int64_t           lastActiveMs = 0;   ///< 最後にセッションが 1 本以上あった時刻
	int               logSink = -1;
	std::string       token;

	// --- HTTP ---
	Response dispatch(Request& req, HttpServer::Conn& conn);
	Response dispatchRoute(const Route& route, Request& req);
	Response serveWebRoot(const Request& req);
	Response serveMount(const DirMount& m, const Request& req);

	// --- 内蔵ルート ---
	bool     isBuiltin(const std::string& path) const;
	Response builtin(Request& req, HttpServer::Conn& conn);
	Response handleInfo(const Request& req);
	Response handleHello(const Request& req);
	Response handlePoll(Request& req, HttpServer::Conn& conn);
	Response handleResult(const Request& req);
	Response handleBye(const Request& req);
	Response handleHeartbeat(const Request& req);
	Response handleRepl(const Request& req);
	Response handleSse(Request& req, HttpServer::Conn& conn,
	                   const std::string& channel, bool withBacklog);

	// --- 認証 ---
	bool authorized(const Request& req) const;
	bool originAllowed(const Request& req) const;

	// --- ライフサイクル ---
	bool shouldAutoExit(size_t aliveSessions) const;
	void installLogSink();
	void uninstallLogSink();
};

} // namespace appserve
