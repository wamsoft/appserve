//---------------------------------------------------------------------------
// HttpServer 実装
//---------------------------------------------------------------------------
#include "http/server.h"
#include "appserve/log.h"
#include "core/util.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace appserve {

namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes   = 512ull * 1024 * 1024;   // ローカルツール想定の安全上限

enum class ReadResult { Ok, Closed, BadRequest, TooLarge, Unsupported };

//---------------------------------------------------------------------------
// buf に溜まったバイト列から HTTP リクエストを 1 件切り出す。
// 足りなければソケットから追加で読む。消費した分は buf から取り除く
// (keep-alive / パイプライン化に備えて残りは保持する)。
ReadResult readRequest(sock_t s, std::string& buf, Request& req, bool& keepAlive)
{
	size_t hdrEnd = buf.find("\r\n\r\n");
	char tmp[8192];
	while (hdrEnd == std::string::npos) {
		if (buf.size() > kMaxHeaderBytes) return ReadResult::TooLarge;
#ifdef _WIN32
		int n = ::recv(s, tmp, (int)sizeof(tmp), 0);
#else
		ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
#endif
		if (n <= 0) return ReadResult::Closed;
		buf.append(tmp, (size_t)n);
		hdrEnd = buf.find("\r\n\r\n");
	}

	// --- リクエスト行 ---
	size_t lineEnd = buf.find("\r\n");
	std::string reqline = buf.substr(0, lineEnd);
	size_t sp1 = reqline.find(' ');
	size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : reqline.find(' ', sp1 + 1);
	if (sp1 == std::string::npos) return ReadResult::BadRequest;

	req.method = reqline.substr(0, sp1);
	std::string target = (sp2 == std::string::npos)
		? reqline.substr(sp1 + 1)
		: reqline.substr(sp1 + 1, sp2 - sp1 - 1);
	std::string version = (sp2 == std::string::npos) ? "HTTP/1.0" : reqline.substr(sp2 + 1);

	size_t qpos = target.find('?');
	req.query = (qpos == std::string::npos) ? std::string() : target.substr(qpos + 1);
	req.path  = urlDecode(qpos == std::string::npos ? target : target.substr(0, qpos), false);

	// --- ヘッダ ---
	req.headers.clear();
	size_t pos = lineEnd + 2;
	while (pos < hdrEnd) {
		size_t eol = buf.find("\r\n", pos);
		if (eol == std::string::npos || eol > hdrEnd) break;
		size_t colon = buf.find(':', pos);
		if (colon != std::string::npos && colon < eol) {
			std::string key = buf.substr(pos, colon - pos);
			size_t vs = colon + 1;
			while (vs < eol && (buf[vs] == ' ' || buf[vs] == '\t')) ++vs;
			req.headers.add(std::move(key), buf.substr(vs, eol - vs));
		}
		pos = eol + 2;
	}

	// --- keep-alive 判定 ---
	std::string conn = util::toLower(req.headers.get("connection"));
	if (version == "HTTP/1.0") keepAlive = (conn.find("keep-alive") != std::string::npos);
	else                       keepAlive = (conn.find("close") == std::string::npos);

	// --- ボディ ---
	if (util::toLower(req.headers.get("transfer-encoding")).find("chunked") !=
	    std::string::npos) {
		// ローカル UI からは使われない。使いたくなったらここに実装を足す。
		return ReadResult::Unsupported;
	}
	size_t clen = 0;
	std::string cl = req.headers.get("content-length");
	if (!cl.empty()) {
		long long v = strtoll(cl.c_str(), nullptr, 10);
		if (v < 0) return ReadResult::BadRequest;
		if ((size_t)v > kMaxBodyBytes) return ReadResult::TooLarge;
		clen = (size_t)v;
	}

	size_t bodyStart = hdrEnd + 4;
	while (buf.size() - bodyStart < clen) {
#ifdef _WIN32
		int n = ::recv(s, tmp, (int)sizeof(tmp), 0);
#else
		ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
#endif
		if (n <= 0) return ReadResult::Closed;
		buf.append(tmp, (size_t)n);
	}
	req.body.assign(buf, bodyStart, clen);
	buf.erase(0, bodyStart + clen);
	return ReadResult::Ok;
}

//---------------------------------------------------------------------------
bool sendResponse(sock_t s, const Response& r, bool keepAlive)
{
	std::string head;
	head.reserve(256 + r.body.size());
	head += "HTTP/1.1 ";
	head += std::to_string(r.status);
	head += ' ';
	head += statusText(r.status);
	head += "\r\n";
	if (!r.mime.empty()) {
		head += "Content-Type: ";
		head += r.mime;
		head += "\r\n";
	}
	head += "Content-Length: ";
	head += std::to_string(r.body.size());
	head += "\r\n";
	for (const auto& kv : r.headers.all()) {
		// Content-Type / Content-Length / Connection は上で確定済み
		if (kv.first == "content-type" || kv.first == "content-length" ||
		    kv.first == "connection") continue;
		head += kv.first;
		head += ": ";
		head += kv.second;
		head += "\r\n";
	}
	if (!r.headers.has("cache-control")) head += "Cache-Control: no-cache\r\n";
	head += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	head += "\r\n";

	if (!net::sendAll(s, head.data(), head.size())) return false;
	if (!r.body.empty() && !net::sendAll(s, r.body.data(), r.body.size())) return false;
	return true;
}

} // anonymous

//---------------------------------------------------------------------------
HttpServer::HttpServer() {}

HttpServer::~HttpServer() { stop(); }

std::string HttpServer::url() const
{
	if (!active()) return std::string();
	return "http://" + url_host_ + ":" + std::to_string(port_) + "/";
}

//---------------------------------------------------------------------------
bool HttpServer::start(const std::string& host_in, int port, Dispatcher dispatcher)
{
	if (running_.load(std::memory_order_acquire)) return true;
	if (port < 0 || port >= 65536) {
		logE("HttpServer: invalid port " + std::to_string(port));
		return false;
	}
	if (!net::globalInit()) {
		logE("HttpServer: socket subsystem init failed");
		return false;
	}

	host_       = host_in.empty() ? std::string("127.0.0.1") : host_in;
	dispatcher_ = std::move(dispatcher);

	listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (listen_ == APPSERVE_SOCK_INVALID) {
		logE("HttpServer: socket() failed");
		net::globalCleanup();
		return false;
	}
	int one = 1;
	::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

	// バインドアドレス解決。"0.0.0.0" / "*" は全 IF、解決できない名前は loopback。
	unsigned long bindaddr;
	bool loopbackOnly;
	if (host_ == "0.0.0.0" || host_ == "*") {
		bindaddr     = htonl(INADDR_ANY);
		loopbackOnly = false;
	} else if (host_ == "localhost") {
		bindaddr     = htonl(INADDR_LOOPBACK);
		loopbackOnly = true;
	} else {
		struct in_addr ia;
		if (inet_pton(AF_INET, host_.c_str(), &ia) == 1) {
			bindaddr     = ia.s_addr;
			loopbackOnly = (ia.s_addr == htonl(INADDR_LOOPBACK));
		} else {
			logW("HttpServer: cannot resolve '" + host_ + "', falling back to 127.0.0.1");
			host_        = "127.0.0.1";
			bindaddr     = htonl(INADDR_LOOPBACK);
			loopbackOnly = true;
		}
	}

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons((unsigned short)port);
	addr.sin_addr.s_addr = bindaddr;

	if (::bind(listen_, (sockaddr*)&addr, sizeof(addr)) != 0 ||
	    ::listen(listen_, 16) != 0) {
		logE("HttpServer: bind/listen failed on " + host_ + ":" + std::to_string(port));
		net::closeSocket(listen_);
		listen_ = APPSERVE_SOCK_INVALID;
		net::globalCleanup();
		return false;
	}

	// port=0 で OS に割り当てさせた場合、実ポートを取り出す
	{
		sockaddr_in actual;
		std::memset(&actual, 0, sizeof(actual));
#ifdef _WIN32
		int len = sizeof(actual);
#else
		socklen_t len = sizeof(actual);
#endif
		if (::getsockname(listen_, (sockaddr*)&actual, &len) == 0)
			port_ = ntohs(actual.sin_port);
		else
			port_ = port;
	}

	// 0.0.0.0 のままでは「どこへ繋げばいいか」が分からないので、表示用は
	// 外向き実 IPv4 に解決する。
	if (host_ == "0.0.0.0" || host_ == "*") {
		std::string outward = net::outwardIPv4();
		url_host_ = outward.empty() ? std::string("127.0.0.1") : outward;
	} else {
		url_host_ = host_;
	}

	if (!loopbackOnly) {
		logW("HttpServer: binding non-loopback address (" + host_ +
		     ") — the API is reachable from the network. Use only on trusted LANs.");
	}

	running_.store(true, std::memory_order_release);
	accept_thread_ = std::thread(&HttpServer::acceptLoop, this);
	return true;
}

//---------------------------------------------------------------------------
void HttpServer::stop()
{
	if (!running_.exchange(false)) return;

	// listen を閉じて accept を解除する
	if (listen_ != APPSERVE_SOCK_INVALID) {
		net::closeSocket(listen_);
		listen_ = APPSERVE_SOCK_INVALID;
	}
	if (accept_thread_.joinable()) accept_thread_.join();

	// 接続スレッドは detach しているので、抜けるのを少しだけ待つ。
	// (SSE / ロングポーリングは running_ = false で自発的に終了する)
	{
		std::unique_lock<std::mutex> lk(conns_mu_);
		conns_cv_.wait_for(lk, std::chrono::seconds(3),
		                   [&] { return conns_.load(std::memory_order_relaxed) == 0; });
	}
	net::globalCleanup();
}

//---------------------------------------------------------------------------
void HttpServer::acceptLoop()
{
	while (running_.load(std::memory_order_acquire)) {
		sockaddr_in cli;
		std::memset(&cli, 0, sizeof(cli));
#ifdef _WIN32
		int clen = sizeof(cli);
#else
		socklen_t clen = sizeof(cli);
#endif
		sock_t c = ::accept(listen_, (sockaddr*)&cli, &clen);
		if (c == APPSERVE_SOCK_INVALID) {
			if (!running_.load(std::memory_order_acquire)) break;
			continue;
		}
		unsigned long a = ntohl(cli.sin_addr.s_addr);
		char rbuf[32];
		std::snprintf(rbuf, sizeof(rbuf), "%lu.%lu.%lu.%lu",
		              (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);

		conns_.fetch_add(1, std::memory_order_relaxed);
		std::thread(&HttpServer::handleConnection, this, c, std::string(rbuf)).detach();
	}
}

//---------------------------------------------------------------------------
void HttpServer::handleConnection(sock_t s, std::string remote)
{
	net::setNoDelay(s);
	net::setRecvTimeout(s, keepalive_ms_);

	std::string buf;
	while (running_.load(std::memory_order_acquire)) {
		Request req;
		req.remote = remote;
		bool keepAlive = true;

		ReadResult rr = readRequest(s, buf, req, keepAlive);
		if (rr == ReadResult::Closed) break;
		if (rr != ReadResult::Ok) {
			int status = (rr == ReadResult::TooLarge)    ? 413 :
			             (rr == ReadResult::Unsupported) ? 501 : 400;
			sendResponse(s, Response::error(status, statusText(status)), false);
			errors_.fetch_add(1, std::memory_order_relaxed);
			break;
		}

		requests_.fetch_add(1, std::memory_order_relaxed);

		Conn conn;
		conn.sock    = s;
		conn.remote  = remote;
		conn.running = &running_;

		Response resp;
		try {
			resp = dispatcher_(req, conn);
		} catch (const std::exception& e) {
			logE(std::string("dispatch threw: ") + e.what());
			resp = Response::error(500, e.what());
		} catch (...) {
			logE("dispatch threw unknown exception");
			resp = Response::error(500, "internal error");
		}

		if (conn.hijacked) break;          // SSE 等。応答は書き済み。
		if (resp.status >= 400) errors_.fetch_add(1, std::memory_order_relaxed);
		if (!sendResponse(s, resp, keepAlive)) break;
		if (!keepAlive) break;
	}

	net::closeSocket(s);
	if (conns_.fetch_sub(1, std::memory_order_relaxed) == 1) {
		std::lock_guard<std::mutex> lk(conns_mu_);
		conns_cv_.notify_all();
	}
}

} // namespace appserve
