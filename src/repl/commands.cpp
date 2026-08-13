//---------------------------------------------------------------------------
// 内蔵 REPL コマンド
//
// サーバ側の状態確認 (.info / .routes / .sessions / .stat / .get ...) と、
// ブラウザ側の観測・操作 (.b eval / .b dom / .b click ...) の 2 系統。
// 後者があることで、エージェントは UI の実状態を見ながらフロントとバック
// エンドの両方を直せる。
//---------------------------------------------------------------------------
#include "repl/repl.h"
#include "core/app_impl.h"

#include "appserve/log.h"
#include "core/util.h"

#include <algorithm>
#include <stdexcept>

namespace appserve {

namespace {

//---------------------------------------------------------------------------
/// BrowserChannel::call の結果を REPL 表示用の文字列にする。
/// 失敗は例外にして呼び出し側 (Repl::execute) が error として拾う。
std::string renderCallResult(const Json& r)
{
	if (!r["ok"].asBool(false)) {
		std::string e = r["error"].asStr();
		throw std::runtime_error(e.empty() ? "browser command failed" : e);
	}
	const Json& v = r["value"];
	if (v.isNull()) return "(null)";
	if (v.isStr())  return v.asStr();
	return v.dump(2);
}

/// "sel value..." を 2 つに割る (value 側は空白を含んでよい)
void splitFirst(const std::string& args, std::string& first, std::string& rest)
{
	first = util::nextToken(args, rest);
}

} // anonymous

//---------------------------------------------------------------------------
void registerBuiltinReplCommands(App& app, Registry& registry)
{
	App::Impl& d = app.impl();
	registry.setOwner("appserve");

	//-----------------------------------------------------------------------
	registry.replCommand("help", "list commands", [&app](const std::string&) {
		return app.impl().repl->helpText();
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("info", "server url / token / uptime", [&app, &d](const std::string&) {
		const Options& o = app.options();
		std::string s;
		s += "app          : " + o.appName + " " + o.appVersion + "\n";
		s += "url          : " + app.url() + "\n";
		s += "url (+token) : " + app.urlWithToken() + "\n";
		s += "token        : " + (o.useToken ? app.token() : std::string("(disabled)")) + "\n";
		s += "web root     : " + std::string(d.webroot.kindName()) + " " +
		     d.webroot.location() + "\n";
		s += "uptime       : " +
		     std::to_string((util::nowMs() - d.startMs) / 1000) + "s\n";
		s += "idle timeout : " + (o.idleTimeout > 0
			? std::to_string(o.idleTimeout) + "s" : std::string("disabled")) + "\n";
		s += "write access : " + std::string(o.allowWrite ? "enabled" : "disabled") + "\n";
		return s;
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("routes", "list registered routes and mounts",
	                     [&d](const std::string&) {
		auto routes = d.registry.routes();
		std::sort(routes.begin(), routes.end(),
		          [](const std::shared_ptr<const Route>& a,
		             const std::shared_ptr<const Route>& b) { return a->prefix < b->prefix; });

		std::string s = "routes:\n";
		for (const auto& r : routes) {
			std::string left = "  " + r->prefix;
			while (left.size() < 30) left += ' ';
			s += left + (r->affinity == Affinity::Main ? "[main] " : "[any]  ") +
			     r->owner + "\n";
		}
		auto mounts = d.registry.mounts();
		if (!mounts.empty()) {
			s += "static mounts:\n";
			for (const auto& m : mounts) {
				std::string left = "  " + m->prefix;
				while (left.size() < 30) left += ' ';
				s += left + m->dir + "  (" + m->owner + ")\n";
			}
		}
		s += "built-in: /_app/info /_app/hello /_app/poll /_app/result /_app/bye"
		     " /_app/hb /_app/events /_app/sub/<ch> /_app/repl\n";
		return s;
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("web", "show how the UI assets were resolved",
	                     [&d](const std::string& args) {
		std::string s = std::string("kind     : ") + d.webroot.kindName() + "\n";
		s += "location : " + d.webroot.location() + "\n";
		if (util::trim(args) == "list" || util::trim(args) == "-l") {
			auto files = d.webroot.list();
			std::sort(files.begin(), files.end());
			s += "files    : " + std::to_string(files.size()) + "\n";
			for (const auto& f : files) s += "  " + f + "\n";
		} else {
			s += "(use '.web list' to list the files)\n";
		}
		return s;
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("sessions", "list connected browser sessions",
	                     [&d](const std::string&) {
		auto list = d.sessions.all();
		if (list.empty()) return std::string("(no browser connected)\n");
		std::string s = "sessions:\n";
		int64_t now = util::nowMs();
		for (const auto& sn : list) {
			int64_t seen;
			size_t  pending;
			uint64_t delivered;
			{
				std::lock_guard<std::mutex> lk(sn->mu);
				seen      = sn->lastSeenMs;
				pending   = sn->outbox.size();
				delivered = sn->delivered;
			}
			s += "  " + sn->sid +
			     "  seen " + std::to_string((now - seen) / 1000) + "s ago" +
			     "  pending " + std::to_string(pending) +
			     "  delivered " + std::to_string(delivered) +
			     "  " + sn->remote + "\n";
			if (!sn->ua.empty()) s += "      " + sn->ua + "\n";
		}
		return s;
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("stat", "request / connection counters", [&d](const std::string&) {
		std::string s;
		s += "uptime       : " + std::to_string((util::nowMs() - d.startMs) / 1000) + "s\n";
		s += "requests     : " + std::to_string(d.server.requestCount()) + "\n";
		s += "errors       : " + std::to_string(d.server.errorCount()) + "\n";
		s += "connections  : " + std::to_string(d.server.connectionCount()) + "\n";
		s += "sessions     : " + std::to_string(d.sessions.count()) + "\n";
		s += "sse clients  : " + std::to_string(d.sessions.sseCount()) + "\n";
		s += "pending tasks: " + std::to_string(d.queue.pending()) + "\n";
		s += "await results: " + std::to_string(d.sessions.waiterCount()) + "\n";
		return s;
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("get", "GET <path> through the router",
	                     [&app](const std::string& args) {
		std::string path = util::trim(args);
		if (path.empty()) throw std::runtime_error("usage: .get <path>");
		std::string query;
		size_t q = path.find('?');
		if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }

		Response r = app.dispatchInternal("GET", path, query);
		std::string s = std::to_string(r.status) + " " + statusText(r.status) +
		                "  (" + r.mime + ", " + std::to_string(r.body.size()) + " bytes)\n";
		// JSON なら整形して見せる
		Json j;
		if (Json::parse(r.body, j, nullptr)) s += j.dump(2) + "\n";
		else if (r.body.size() <= 8192)      s += r.body + "\n";
		else                                 s += "(body omitted)\n";
		return s;
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("post", "POST <path> [body] through the router",
	                     [&app](const std::string& args) {
		std::string path, body;
		splitFirst(args, path, body);
		if (path.empty()) throw std::runtime_error("usage: .post <path> [body]");
		std::string query;
		size_t q = path.find('?');
		if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }

		Response r = app.dispatchInternal("POST", path, query, body);
		std::string s = std::to_string(r.status) + " " + statusText(r.status) + "\n";
		Json j;
		if (Json::parse(r.body, j, nullptr)) s += j.dump(2) + "\n";
		else if (!r.body.empty())            s += r.body + "\n";
		return s;
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("push", "broadcast <channel> <text> over SSE",
	                     [&app](const std::string& args) {
		std::string ch, text;
		splitFirst(args, ch, text);
		if (ch.empty()) throw std::runtime_error("usage: .push <channel> <text>");
		app.browser().broadcast(ch, text);
		return "sent to /_app/sub/" + ch + "\n";
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("log", "show or set the log level", [](const std::string& args) {
		std::string v = util::trim(args);
		if (v.empty()) return std::string("log level: ") + logLevelName(logLevel()) + "\n";
		LogLevel lv;
		if (!parseLogLevel(v, lv))
			throw std::runtime_error("unknown level '" + v +
			                         "' (verbose|debug|info|warn|error|off)");
		setLogLevel(lv);
		return std::string("log level: ") + logLevelName(lv) + "\n";
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("open", "open (or re-open) the UI in a browser",
	                     [&app](const std::string& args) {
		std::string u = util::trim(args);
		bool ok = app.openBrowser(u);
		return ok ? "opened\n" : "failed to open a browser\n";
	}, Affinity::Any);

	//-----------------------------------------------------------------------
	// ブラウザ遠隔操作。Affinity::Any にしているのは、結果を待つあいだ
	// メインループを止めないため (ブラウザ側の処理が Main の API を叩く
	// 可能性があり、止めると相互待ちになる)。
	//-----------------------------------------------------------------------
	registry.replCommand("b",
		"browser: eval|dom|text|click|set|state|call|nav|err (see .bhelp)",
		[&app](const std::string& args) {
			std::string sub, rest;
			splitFirst(args, sub, rest);
			if (sub.empty())
				throw std::runtime_error("usage: .b <eval|dom|text|click|set|state|call|nav|err> ...");

			Json arg = Json::object();
			int timeout = 5000;

			if (sub == "eval") {
				if (rest.empty()) throw std::runtime_error("usage: .b eval <javascript>");
				arg.set("code", Json(rest));
			} else if (sub == "dom" || sub == "text" || sub == "click") {
				arg.set("sel", Json(util::trim(rest).empty() ? "body" : util::trim(rest)));
			} else if (sub == "set") {
				std::string sel, value;
				splitFirst(rest, sel, value);
				if (sel.empty()) throw std::runtime_error("usage: .b set <selector> <value>");
				arg.set("sel", Json(sel));
				arg.set("value", Json(value));
			} else if (sub == "state" || sub == "err") {
				// 引数なし
			} else if (sub == "nav") {
				arg.set("path", Json(util::trim(rest)));
			} else if (sub == "call") {
				std::string name, payload;
				splitFirst(rest, name, payload);
				if (name.empty()) throw std::runtime_error("usage: .b call <name> [json]");
				arg.set("name", Json(name));
				Json parsed;
				if (!payload.empty() && Json::parse(payload, parsed, nullptr))
					arg.set("arg", std::move(parsed));
				else if (!payload.empty())
					arg.set("arg", Json(payload));
				timeout = 15000;   // アプリ側の処理は長引きうる
			} else {
				// 未知のサブコマンドはそのままブラウザへ渡す (アプリ拡張用)
				Json parsed;
				if (!rest.empty() && Json::parse(rest, parsed, nullptr)) arg = std::move(parsed);
				else if (!rest.empty()) arg.set("value", Json(rest));
			}

			Json r = app.browser().call(sub, arg, timeout);
			return renderCallResult(r);
		}, Affinity::Any);

	//-----------------------------------------------------------------------
	registry.replCommand("bhelp", "help for the browser (.b) commands",
	                     [](const std::string&) {
		return std::string(
			"browser commands (require a connected browser):\n"
			"  .b eval <js>            evaluate JavaScript, return the result\n"
			"  .b dom [selector]       outerHTML of the first match (default body)\n"
			"  .b text [selector]      innerText of the first match\n"
			"  .b click <selector>     dispatch a real click event\n"
			"  .b set <sel> <value>    set an input value and fire input/change\n"
			"  .b state                app state exposed via app.exposeState()\n"
			"  .b call <name> [json]   invoke a handler registered with app.command()\n"
			"  .b nav <path>           navigate the UI\n"
			"  .b err                  recent front-end errors\n"
			"  .b <other> [json]       forward to a custom app.handler()\n");
	}, Affinity::Any);

	registry.setOwner("app");
	(void)d;
}

} // namespace appserve
