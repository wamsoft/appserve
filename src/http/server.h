//---------------------------------------------------------------------------
// HttpServer — ミニマムな HTTP/1.1 サーバ
//
// 吉里吉里Z の ReplWebServer と同じ thread-per-connection モデル。ローカル
// ツール用途では接続数が高々十数本なので、これで十分かつ実装が最小になる。
// ただし SSE / ロングポーリングで接続を長く保持するのと、ブラウザが小さな
// API を何度も叩くため、keep-alive には対応する (毎回 close だと Windows で
// TIME_WAIT が積み上がる)。
//---------------------------------------------------------------------------
#pragma once
#include "http/socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "appserve/http.h"

namespace appserve {

class HttpServer {
public:
	//-----------------------------------------------------------------------
	/// ディスパッチャに渡される接続ハンドル。
	/// SSE のようにレスポンスを自前でストリーミングしたい場合は、raw 送信を
	/// 行ったうえで hijacked = true にする (サーバは以降なにも書かず接続を閉じる)。
	struct Conn {
		sock_t      sock = APPSERVE_SOCK_INVALID;
		std::string remote;
		bool        hijacked = false;
		/// サーバが停止したか (SSE ループの終了判定に使う)
		const std::atomic<bool>* running = nullptr;

		bool sendRaw(const char* p, size_t n) const { return net::sendAll(sock, p, n); }
		bool sendStr(const std::string& s) const { return net::sendAll(sock, s.data(), s.size()); }
		bool serverRunning() const {
			return running && running->load(std::memory_order_acquire);
		}
	};

	using Dispatcher = std::function<Response(Request&, Conn&)>;

	HttpServer();
	~HttpServer();

	/// listen を開始する。port = 0 なら OS が空きポートを割り当てる。
	/// 成功したら true (実ポートは port() で取得)。
	bool start(const std::string& host, int port, Dispatcher dispatcher);
	void stop();

	bool               active() const { return running_.load(std::memory_order_acquire); }
	int                port() const { return port_; }
	const std::string& bindHost() const { return host_; }
	/// URL 表示用ホスト (0.0.0.0 バインド時は外向き実 IP に解決済み)
	const std::string& urlHost() const { return url_host_; }
	std::string        url() const;

	// --- 統計 ---
	uint64_t requestCount() const { return requests_.load(std::memory_order_relaxed); }
	uint64_t errorCount() const { return errors_.load(std::memory_order_relaxed); }
	int      connectionCount() const { return conns_.load(std::memory_order_relaxed); }

	/// keep-alive の待ち時間 (ms)。この間リクエストが来なければ接続を閉じる。
	void setKeepAliveTimeout(int ms) { keepalive_ms_ = ms; }

private:
	void acceptLoop();
	void handleConnection(sock_t s, std::string remote);

	std::atomic<bool>       running_{false};
	sock_t                  listen_ = APPSERVE_SOCK_INVALID;
	std::thread             accept_thread_;
	std::string             host_;
	std::string             url_host_;
	int                     port_ = 0;
	int                     keepalive_ms_ = 65000;
	Dispatcher              dispatcher_;

	std::atomic<uint64_t>   requests_{0};
	std::atomic<uint64_t>   errors_{0};
	std::atomic<int>        conns_{0};
	std::mutex              conns_mu_;
	std::condition_variable conns_cv_;
};

} // namespace appserve
