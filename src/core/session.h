//---------------------------------------------------------------------------
// SessionManager — ブラウザセッション / コマンド往復 / SSE 購読の管理
//
// セッション = ブラウザのタブ 1 つ。GET /_app/hello で発行し、poll / SSE /
// hb の到達で lastSeen を更新する。生存セッションが 0 になったことが
// アイドル自動終了の判定材料になる。
//---------------------------------------------------------------------------
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "appserve/json.h"

namespace appserve {

//---------------------------------------------------------------------------
/// サーバ → ブラウザ へ送るコマンド 1 件
struct PendingCmd {
	uint64_t    id = 0;      ///< 0 = 結果不要 (post)
	std::string cmd;
	Json        arg;
};

//---------------------------------------------------------------------------
class Session {
public:
	std::string sid;
	std::string ua;
	std::string remote;
	int64_t     createdMs = 0;

	std::mutex              mu;
	std::condition_variable cv;
	std::deque<PendingCmd>  outbox;
	int64_t                 lastSeenMs = 0;
	bool                    closed = false;
	uint64_t                delivered = 0;   ///< 配信済みコマンド数 (統計)
};

//---------------------------------------------------------------------------
/// BrowserChannel::call() の結果待ちスロット
struct CallWaiter {
	std::mutex              mu;
	std::condition_variable cv;
	bool                    done = false;
	bool                    ok = false;
	Json                    value;
	std::string             error;
};

//---------------------------------------------------------------------------
/// SSE 購読 1 本
struct SseClient {
	std::mutex              mu;
	std::condition_variable cv;
	std::deque<std::string> queue;    ///< 送信待ちフレーム ("data: ...\n\n" 済み)
	std::string             channel;
	bool                    closed = false;
};

//---------------------------------------------------------------------------
class SessionManager {
public:
	// --- セッション ---
	std::shared_ptr<Session> create(const std::string& ua, const std::string& remote);
	std::shared_ptr<Session> find(const std::string& sid);
	/// 最も最近見たセッション (call() の既定の宛先)
	std::shared_ptr<Session> newest();
	void   touch(const std::string& sid);
	void   remove(const std::string& sid);
	/// ttlSec を超えて音沙汰の無いセッションを削除する。残数を返す。
	size_t reap(int ttlSec);
	size_t count();
	std::vector<std::shared_ptr<Session>> all();
	/// 全セッションを起こして閉じる (シャットダウン時)
	void   closeAll();

	// --- コマンド往復 ---
	uint64_t nextCmdId() { return ++cmd_seq_; }
	void     addWaiter(uint64_t id, std::shared_ptr<CallWaiter> w);
	/// 取り出して登録解除する (見つからなければ nullptr)
	std::shared_ptr<CallWaiter> takeWaiter(uint64_t id);
	size_t   waiterCount();

	// --- SSE ---
	std::shared_ptr<SseClient> addSse(const std::string& channel);
	void   removeSse(const std::shared_ptr<SseClient>& c);
	void   pushFrame(const std::string& channel, const std::string& frame);
	void   closeAllSse();
	size_t sseCount();

	// --- ログのバックログ (/_app/events 接続時にまとめて流す) ---
	void                     pushLog(const std::string& json);
	std::vector<std::string> logBacklog();

private:
	std::mutex                                     mu_;
	std::vector<std::shared_ptr<Session>>          sessions_;

	std::mutex                                     waiters_mu_;
	std::vector<std::pair<uint64_t, std::shared_ptr<CallWaiter>>> waiters_;
	std::atomic<uint64_t>                          cmd_seq_{0};

	std::mutex                                     sse_mu_;
	std::vector<std::shared_ptr<SseClient>>        sse_;

	std::mutex                                     ring_mu_;
	std::deque<std::string>                        ring_;
	static const size_t                            kRingMax = 2000;
};

/// payload (改行含み可) を SSE 1 イベントぶんのフレームへ整形する
std::string sseFrame(const std::string& payload);

} // namespace appserve
