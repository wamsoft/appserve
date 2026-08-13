//---------------------------------------------------------------------------
// BrowserChannel — サーバ → ブラウザ の一方向 push とコマンド往復
//
// ブラウザ側の appserve.js が /_app/poll をロングポーリングしており、
// post() / call() で積んだコマンドを即座に受け取って実行する。call() は
// ブラウザからの実行結果 (POST /_app/result) を待って返す。
//
// これにより REPL / エージェントから「ブラウザ側の状態を観測して操作する」
// ことができる (.b eval / .b dom / .b click ...)。
//
// call() はメインスレッドから呼んでも安全 (/_app/* は Affinity::Any なので
// メインスレッドの応答を待たない)。ただしその間メインループは停止する。
//---------------------------------------------------------------------------
#pragma once
#include <string>
#include <vector>
#include "appserve/json.h"

namespace appserve {

class SessionManager;

class BrowserChannel {
public:
	explicit BrowserChannel(SessionManager& sessions);

	/// 全セッションへコマンドを積む (結果は待たない)
	void post(const std::string& cmd, const Json& arg = Json());

	/// 1 セッションへコマンドを送り、結果を待つ。
	/// sid が空なら最も新しく生存しているセッションが対象。
	/// 戻り値: %[ ok, value, error ]。タイムアウト/接続なしは ok=false。
	Json call(const std::string& cmd, const Json& arg = Json(),
	          int timeoutMs = 5000, const std::string& sid = std::string());

	/// SSE チャネル /_app/sub/<channel> の購読者へ配信する (改行可)
	void broadcast(const std::string& channel, const std::string& payload);
	/// JSON を broadcast する糖衣
	void broadcastJson(const std::string& channel, const Json& payload);

	/// ブラウザ側から返ってきた結果を受け取る (内部用)
	void deliverResult(uint64_t id, bool ok, const Json& value, const std::string& error);

private:
	SessionManager& sessions_;
};

} // namespace appserve
