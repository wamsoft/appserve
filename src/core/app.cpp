//---------------------------------------------------------------------------
// App 実装
//---------------------------------------------------------------------------
#include "core/app_impl.h"

#include "appserve/log.h"
#include "browser/launcher.h"
#include "core/util.h"
#include "plugin/loader.h"
#include "repl/repl.h"

#include <chrono>
#include <csignal>
#include <cstring>

namespace appserve {

namespace {

/// 終了シグナル (Ctrl+C) を受け取るための単一インスタンス参照。
/// appserve は 1 プロセス 1 App の前提。
App* g_signal_target = nullptr;

void onSignal(int)
{
	if (g_signal_target) g_signal_target->requestShutdown(0);
}

/// SSE ハンドシェイク済みかどうかに関わらず送るヘッダ
const char* kSseHeader =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/event-stream; charset=utf-8\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: keep-alive\r\n"
	"X-Accel-Buffering: no\r\n\r\n";

} // anonymous

//---------------------------------------------------------------------------
App::Impl::Impl(App& o)
	: owner(o), registry(o), browser(sessions)
{
}

App::Impl::~Impl() = default;

//---------------------------------------------------------------------------
// App
//---------------------------------------------------------------------------
App::App() : impl_(new Impl(*this)) {}

App::~App()
{
	if (g_signal_target == this) g_signal_target = nullptr;
}

void App::addOption(OptionSpec spec)
{
	impl_->customOptions.push_back(std::move(spec));
}

void App::addModule(std::unique_ptr<IModule> module)
{
	if (module) impl_->modules.push_back(std::move(module));
}

ApiRegistry&    App::registry() { return impl_->registry; }
BrowserChannel& App::browser()  { return impl_->browser; }

bool App::active() const { return impl_->server.active(); }
int  App::port() const   { return impl_->server.port(); }

std::string App::url() const
{
	return impl_->server.url();
}

std::string App::urlWithToken() const
{
	std::string u = url();
	if (u.empty()) return u;
	if (opt_.useToken && !impl_->token.empty()) u += "?t=" + impl_->token;
	return u;
}

const std::string& App::token() const { return impl_->token; }

void App::requestShutdown(int code)
{
	impl_->exitCode.store(code, std::memory_order_relaxed);
	impl_->running.store(false, std::memory_order_release);
	// メインループが drain の待機中でも即座に抜けられるよう空タスクで起こす
	impl_->queue.post([] {});
}

//---------------------------------------------------------------------------
// 認証
//---------------------------------------------------------------------------
bool App::Impl::authorized(const Request& req) const
{
	if (!owner.options().useToken) return true;
	if (token.empty()) return true;
	std::string t = req.headers.get("x-app-token");
	if (t.empty()) t = req.param("t");
	// 定数時間比較まではしない (ローカル loopback 前提、タイミング攻撃の経路が無い)
	return t == token;
}

bool App::Impl::originAllowed(const Request& req) const
{
	// Origin が無い = 同一オリジンの通常ナビゲーション or 非ブラウザ (curl)。
	// ブラウザの cross-origin fetch には必ず Origin が付くので、これだけで
    // 悪意ある Web ページからの CSRF は塞げる。
	std::string origin = req.headers.get("origin");
	if (origin.empty()) return true;

	int p = server.port();
	const std::string suffix = ":" + std::to_string(p);
	if (origin == "http://127.0.0.1" + suffix) return true;
	if (origin == "http://localhost" + suffix) return true;
	if (origin == "http://[::1]" + suffix) return true;
	if (!server.urlHost().empty() && origin == "http://" + server.urlHost() + suffix)
		return true;
	return false;
}

//---------------------------------------------------------------------------
// ディスパッチ
//---------------------------------------------------------------------------
bool App::Impl::isBuiltin(const std::string& path) const
{
	return util::startsWith(path, "/_app/");
}

Response App::Impl::dispatch(Request& req, HttpServer::Conn& conn)
{
	const std::string& p = req.path;

	const bool builtinRoute = isBuiltin(p);
	auto route = builtinRoute ? nullptr : registry.matchRoute(p);
	auto mount = (builtinRoute || route) ? nullptr : registry.matchMount(p);

	// 内蔵ルート / 登録ルート / マウントは Origin + トークンを検証する。
	// 静的 UI 配信は素通し (ページ自体を読ませないと token を渡せないため)。
	if (builtinRoute || route || mount) {
		if (!originAllowed(req)) {
			logW("rejected cross-origin request: " + req.headers.get("origin") +
			     " -> " + p);
			return Response::error(403, "cross-origin request rejected");
		}
		if (!authorized(req)) return Response::error(401, "invalid or missing token");
	}

	if (builtinRoute) return builtin(req, conn);

	if (route) {
		req.prefix = route->prefix;
		req.suffix = p.substr(route->prefix.size());
		return dispatchRoute(*route, req);
	}

	if (mount) {
		if (req.method != "GET" && req.method != "HEAD")
			return Response::error(405, "method not allowed");
		return serveMount(*mount, req);
	}

	if (req.method == "GET" || req.method == "HEAD") return serveWebRoot(req);
	return Response::error(404, "not found");
}

//---------------------------------------------------------------------------
Response App::Impl::dispatchRoute(const Route& route, Request& req)
{
	Response resp;
	auto invoke = [&] {
		try {
			resp = route.handler(req);
		} catch (const std::exception& e) {
			logE("handler error (" + route.prefix + "): " + e.what());
			resp = Response::error(500, e.what());
		} catch (...) {
			logE("handler error (" + route.prefix + "): unknown exception");
			resp = Response::error(500, "unknown error in handler");
		}
	};

	if (route.affinity == Affinity::Any) {
		invoke();
		return resp;
	}
	// メインスレッドへ運んで直列実行する
	if (!queue.submit(invoke)) return Response::error(503, "server is shutting down");
	return resp;
}

//---------------------------------------------------------------------------
Response App::Impl::serveWebRoot(const Request& req)
{
	if (!webroot.valid()) {
		return Response::error(404,
			"no web root (looked for ./web, <exe>/web, <exe>/web.zip, embedded)");
	}
	std::string rel = req.path;
	if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
	if (rel.empty() || rel.back() == '/') rel += "index.html";

	if (!WebRoot::safeRelPath(rel)) return Response::error(403, "forbidden path");

	std::string data;
	if (!webroot.read(rel, data)) {
		// SPA モードでは未知パスを index.html に流す (クライアントルーティング)
		if (owner.options().spa && rel.find('.') == std::string::npos &&
		    webroot.read("index.html", data)) {
			return Response::bytes(std::move(data), "text/html; charset=utf-8");
		}
		return Response::error(404, "not found: " + rel);
	}
	return Response::bytes(std::move(data), mimeForPath(rel));
}

Response App::Impl::serveMount(const DirMount& m, const Request& req)
{
	std::string rel = req.path.substr(m.prefix.size());
	if (rel.empty() || rel.back() == '/') rel += "index.html";
	if (!WebRoot::safeRelPath(rel)) return Response::error(403, "forbidden path");

	std::string data;
	if (!util::readFile(util::joinPath(m.dir, rel), data, 256ull * 1024 * 1024))
		return Response::error(404, "not found: " + rel);
	return Response::bytes(std::move(data), mimeForPath(rel));
}

//---------------------------------------------------------------------------
// 内蔵ルート
//---------------------------------------------------------------------------
Response App::Impl::builtin(Request& req, HttpServer::Conn& conn)
{
	const std::string& p = req.path;

	if (p == "/_app/info"   && req.method == "GET")  return handleInfo(req);
	if (p == "/_app/hello"  && req.method == "GET")  return handleHello(req);
	if (p == "/_app/poll"   && req.method == "GET")  return handlePoll(req, conn);
	if (p == "/_app/result" && req.method == "POST") return handleResult(req);
	if (p == "/_app/bye"    && (req.method == "POST" || req.method == "GET"))
		return handleBye(req);
	if (p == "/_app/hb"     && (req.method == "POST" || req.method == "GET"))
		return handleHeartbeat(req);
	if (p == "/_app/repl"   && req.method == "POST") return handleRepl(req);
	if (p == "/_app/events" && req.method == "GET")
		return handleSse(req, conn, "log", true);
	if (util::startsWith(p, "/_app/sub/") && req.method == "GET") {
		std::string ch = p.substr(strlen("/_app/sub/"));
		if (ch.empty()) return Response::error(400, "channel required");
		return handleSse(req, conn, ch, false);
	}
	return Response::error(404, "unknown built-in route: " + p);
}

//---------------------------------------------------------------------------
Response App::Impl::handleInfo(const Request&)
{
	const Options& o = owner.options();
	Json j = Json::object();
	j.set("app",       Json(o.appName));
	j.set("version",   Json(o.appVersion));
	j.set("port",      Json(server.port()));
	j.set("url",       Json(server.url()));
	j.set("uptimeMs",  Json((long long)(util::nowMs() - startMs)));
	j.set("webRoot",   Json(std::string(webroot.kindName())));
	j.set("webRootAt", Json(webroot.location()));
	j.set("allowWrite", Json(o.allowWrite));
	j.set("sessions",  Json((long long)sessions.count()));
	j.set("requests",  Json((long long)server.requestCount()));

	// 派生アプリの UI が機能の有無を見て分岐できるように、登録済みルートの
	// prefix を返す (ハンドラそのものは晒さない)
	Json routes = Json::array();
	for (const auto& r : registry.routes()) routes.push(Json(r->prefix));
	j.set("routes", std::move(routes));
	return Response::json(j);
}

//---------------------------------------------------------------------------
Response App::Impl::handleHello(const Request& req)
{
	auto s = sessions.create(req.headers.get("user-agent"), req.remote);
	logD("session opened: " + s->sid + " (" + req.remote + ")");

	Json j = Json::object();
	j.set("sid",       Json(s->sid));
	j.set("app",       Json(owner.options().appName));
	j.set("version",   Json(owner.options().appVersion));
	j.set("pollMs",    Json(15000));
	j.set("sessionTtl", Json(owner.options().sessionTtl));
	return Response::json(j);
}

//---------------------------------------------------------------------------
Response App::Impl::handlePoll(Request& req, HttpServer::Conn& conn)
{
	std::string sid = req.param("sid");
	auto s = sessions.find(sid);
	if (!s) {
		// セッションが消えている (TTL 切れ / サーバ再起動)。JS 側は hello からやり直す。
		return Response::error(404, "unknown session");
	}

	int64_t waitMs = req.paramInt("wait", 15000);
	if (waitMs < 0) waitMs = 0;
	if (waitMs > 60000) waitMs = 60000;
	const int64_t deadline = util::nowMs() + waitMs;

	std::deque<PendingCmd> batch;
	{
		std::unique_lock<std::mutex> lk(s->mu);
		s->lastSeenMs = util::nowMs();
		while (s->outbox.empty() && !s->closed) {
			int64_t remain = deadline - util::nowMs();
			if (remain <= 0) break;
			if (!conn.serverRunning()) break;
			// サーバ停止やセッション削除に追随できるよう短く刻んで待つ
			int64_t slice = remain < 500 ? remain : 500;
			s->cv.wait_for(lk, std::chrono::milliseconds(slice),
			               [&] { return !s->outbox.empty() || s->closed; });
		}
		batch.swap(s->outbox);
		s->delivered += batch.size();
		s->lastSeenMs = util::nowMs();
	}

	Json cmds = Json::array();
	for (const auto& c : batch) {
		Json e = Json::object();
		e.set("id",  Json((long long)c.id));
		e.set("cmd", Json(c.cmd));
		e.set("arg", c.arg);
		cmds.push(std::move(e));
	}
	Json j = Json::object();
	j.set("cmds", std::move(cmds));
	return Response::json(j);
}

//---------------------------------------------------------------------------
Response App::Impl::handleResult(const Request& req)
{
	const Json& j = req.json();
	if (!j.isObj()) return Response::error(400, "expected a JSON object");

	std::string sid = j["sid"].asStr();
	if (!sid.empty()) sessions.touch(sid);

	uint64_t id = (uint64_t)j["id"].asInt(0);
	if (id == 0) return Response::noContent();   // post() の結果は捨てる

	browser.deliverResult(id, j["ok"].asBool(false), j["value"], j["error"].asStr());
	return Response::noContent();
}

//---------------------------------------------------------------------------
Response App::Impl::handleBye(const Request& req)
{
	std::string sid = req.param("sid");
	if (sid.empty() && !req.body.empty()) sid = req.json()["sid"].asStr();
	if (!sid.empty()) {
		logD("session closed: " + sid);
		sessions.remove(sid);
	}
	return Response::noContent();
}

Response App::Impl::handleHeartbeat(const Request& req)
{
	std::string sid = req.param("sid");
	auto s = sessions.find(sid);
	if (!s) return Response::error(404, "unknown session");
	sessions.touch(sid);
	return Response::noContent();
}

//---------------------------------------------------------------------------
Response App::Impl::handleRepl(const Request& req)
{
	if (!repl) return Response::error(503, "repl is not available");
	std::string line = req.body;
	// JSON ボディ {"cmd":"..."} も受ける (fetch から使いやすいように)
	if (!line.empty() && line[0] == '{') {
		const Json& j = req.json();
		if (j.isObj() && j.has("cmd")) line = j["cmd"].asStr();
	}
	Repl::Result r = repl->execute(line);

	Json j = Json::object();
	j.set("ok",     Json(r.ok));
	j.set("result", Json(r.text));
	j.set("error",  Json(r.error));
	return Response::json(j);
}

//---------------------------------------------------------------------------
Response App::Impl::handleSse(Request& req, HttpServer::Conn& conn,
                              const std::string& channel, bool withBacklog)
{
	// ここから先はレスポンスを自前で書く (ストリーミング)
	conn.hijacked = true;
	if (!conn.sendStr(kSseHeader)) return Response::noContent();

	auto client = sessions.addSse(channel);
	if (withBacklog) {
		for (const auto& j : sessions.logBacklog())
			client->queue.push_back(sseFrame(j));
	}

	// セッションを持っていれば購読中も生きているとみなす
	std::string sid = req.param("sid");

	bool alive = true;
	while (alive && conn.serverRunning()) {
		std::deque<std::string> batch;
		{
			std::unique_lock<std::mutex> lk(client->mu);
			client->cv.wait_for(lk, std::chrono::seconds(10), [&] {
				return !client->queue.empty() || client->closed;
			});
			if (client->closed) alive = false;
			batch.swap(client->queue);
		}
		if (!sid.empty()) sessions.touch(sid);
		if (batch.empty()) {
			// コメント行のハートビート。切断はここで検出する。
			if (!conn.sendStr(":ping\n\n")) break;
			continue;
		}
		std::string out;
		for (auto& f : batch) out += f;
		if (!conn.sendStr(out)) break;
	}
	sessions.removeSse(client);
	return Response::noContent();
}

//---------------------------------------------------------------------------
// ログ sink (SSE へ流す)
//---------------------------------------------------------------------------
void App::Impl::installLogSink()
{
	logSink = addLogSink([this](LogLevel lv, const std::string& line) {
		Json j = Json::object();
		j.set("level", Json(std::string(logLevelName(lv))));
		j.set("text",  Json(line));
		j.set("t",     Json((long long)util::nowMs()));
		std::string payload = j.dump();
		sessions.pushLog(payload);
		sessions.pushFrame("log", sseFrame(payload));
	});
}

void App::Impl::uninstallLogSink()
{
	if (logSink >= 0) {
		appserve::removeLogSink(logSink);
		logSink = -1;
	}
}

//---------------------------------------------------------------------------
// アイドル自動終了
//---------------------------------------------------------------------------
bool App::Impl::shouldAutoExit(size_t aliveSessions) const
{
	const Options& o = owner.options();
	if (o.idleTimeout <= 0) return false;
	// REPL がアタッチされている間は落とさない (開発中にブラウザを閉じても続く)
	if (repl && repl->attached()) return false;
	if (aliveSessions > 0) return false;

	int64_t now = util::nowMs();
	if (now - startMs < (int64_t)o.startupGrace * 1000) return false;
	return (now - lastActiveMs) >= (int64_t)o.idleTimeout * 1000;
}

//---------------------------------------------------------------------------
// 起動
//---------------------------------------------------------------------------
int App::run()
{
	Impl& d = *impl_;
	d.queue.bindMainThread();
	d.startMs      = util::nowMs();
	d.lastActiveMs = d.startMs;

	setLogLevel(opt_.logLevel);
	if (!opt_.logFile.empty() && !setLogFile(opt_.logFile))
		logW("could not open log file: " + opt_.logFile);

	// --- トークン ---
	if (opt_.useToken) {
		d.token = opt_.token.empty() ? util::randomHex(16) : opt_.token;
	}

	// --- UI アセット ---
	if (!d.webroot.resolve(opt_.webRoot)) {
		logW("no web root found — serving API only "
		     "(looked for --web-root, ./web, <exe>/web, <exe>/web.zip, embedded)");
	} else {
		logI(std::string("web root: ") + d.webroot.kindName() + " " + d.webroot.location());
	}

	// --- REPL ---
	d.repl.reset(new Repl(*this, d.registry));
	registerBuiltinReplCommands(*this, d.registry);

	// --- モジュール ---
	for (auto& m : d.modules) {
		d.registry.setOwner(m->name());
		try {
			m->registerApi(d.registry);
		} catch (const std::exception& e) {
			logE(std::string("module '") + m->name() + "' failed to register: " + e.what());
		}
	}
	d.registry.setOwner("app");

	// --- 動的プラグイン ---
	if (opt_.loadPlugins) {
		d.plugins.reset(new PluginLoader(*this, d.registry));
		std::string dir = opt_.pluginDir.empty()
			? util::joinPath(util::executableDir(), "plugins") : opt_.pluginDir;
		d.plugins->loadDirectory(dir);
	}

	// --- サーバ ---
	d.installLogSink();
	bool ok = d.server.start(opt_.host, opt_.port,
		[&d](Request& req, HttpServer::Conn& conn) { return d.dispatch(req, conn); });
	if (!ok) {
		logE("failed to start the HTTP server");
		d.uninstallLogSink();
		return 1;
	}
	d.running.store(true, std::memory_order_release);

	g_signal_target = this;
	std::signal(SIGINT,  onSignal);
	std::signal(SIGTERM, onSignal);

	logI(opt_.appName + " " + opt_.appVersion + " listening on " + d.server.url());
	if (opt_.useToken) logD("token: " + d.token);

	// --- REPL チャネル ---
	if (opt_.repl) d.repl->startStdin();
	if (!opt_.replFile.empty()) d.repl->startFileChannel(opt_.replFile);

	// --- ブラウザ ---
	if (opt_.browser != Options::BrowserMode::None) {
		browser::open(urlWithToken(),
		              opt_.browser == Options::BrowserMode::App,
		              opt_.browserArgs);
	} else {
		logI("open this URL in a browser: " + urlWithToken());
	}

	for (auto& m : d.modules) {
		try {
			m->onStart(*this);
		} catch (const std::exception& e) {
			logE(std::string("module '") + m->name() + "' onStart failed: " + e.what());
		}
	}

	// --- メインループ ---
	while (d.running.load(std::memory_order_acquire)) {
		d.queue.drain(200);
		size_t alive = d.sessions.reap(opt_.sessionTtl);
		if (alive > 0) d.lastActiveMs = util::nowMs();
		if (d.shouldAutoExit(alive)) {
			logI("no browser connected — shutting down");
			requestShutdown(0);
		}
	}

	// --- 終了 ---
	logD("shutting down");
	d.repl->stop();
	d.sessions.closeAll();
	d.sessions.closeAllSse();
	d.server.stop();
	d.queue.shutdown();
	d.queue.drain(0);                 // 残タスクを捨てる前に一度だけ流す

	for (auto it = d.modules.rbegin(); it != d.modules.rend(); ++it) {
		try {
			(*it)->onShutdown();
		} catch (...) {}
	}
	if (d.plugins) d.plugins->unloadAll();
	d.registry.clear();
	d.uninstallLogSink();
	setLogFile("");

	g_signal_target = nullptr;
	return d.exitCode.load(std::memory_order_relaxed);
}

//---------------------------------------------------------------------------
Response App::dispatchInternal(const std::string& method, const std::string& path,
                               const std::string& query, const std::string& body)
{
	Request req;
	req.method = method;
	req.path   = path;
	req.query  = query;
	req.body   = body;
	req.remote = "internal";
	// 内部呼び出しなので認証は通す
	if (opt_.useToken) req.headers.set("x-app-token", impl_->token);

	HttpServer::Conn conn;
	conn.remote = "internal";
	static const std::atomic<bool> kRunning{true};
	conn.running = &kRunning;
	return impl_->dispatch(req, conn);
}

//---------------------------------------------------------------------------
bool App::openBrowser(const std::string& u)
{
	std::string target = u.empty() ? urlWithToken() : u;
	return browser::open(target, opt_.browser != Options::BrowserMode::Default,
	                     opt_.browserArgs);
}

} // namespace appserve
