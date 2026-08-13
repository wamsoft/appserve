//---------------------------------------------------------------------------
// BrowserChannel 実装
//---------------------------------------------------------------------------
#include "appserve/browser.h"
#include "appserve/log.h"
#include "core/session.h"
#include "core/util.h"

#include <chrono>

namespace appserve {

BrowserChannel::BrowserChannel(SessionManager& sessions) : sessions_(sessions) {}

//---------------------------------------------------------------------------
void BrowserChannel::post(const std::string& cmd, const Json& arg)
{
	PendingCmd pc;
	pc.id  = 0;          // 結果不要
	pc.cmd = cmd;
	pc.arg = arg;

	for (auto& s : sessions_.all()) {
		std::lock_guard<std::mutex> lk(s->mu);
		if (s->closed) continue;
		s->outbox.push_back(pc);
		s->cv.notify_all();
	}
}

//---------------------------------------------------------------------------
Json BrowserChannel::call(const std::string& cmd, const Json& arg,
                          int timeoutMs, const std::string& sid)
{
	Json out = Json::object();

	auto s = sid.empty() ? sessions_.newest() : sessions_.find(sid);
	if (!s) {
		out.set("ok", Json(false));
		out.set("error", Json("no browser session is connected"));
		return out;
	}

	uint64_t id = sessions_.nextCmdId();
	auto waiter = std::make_shared<CallWaiter>();
	sessions_.addWaiter(id, waiter);

	{
		std::lock_guard<std::mutex> lk(s->mu);
		if (s->closed) {
			sessions_.takeWaiter(id);
			out.set("ok", Json(false));
			out.set("error", Json("session closed"));
			return out;
		}
		PendingCmd pc;
		pc.id  = id;
		pc.cmd = cmd;
		pc.arg = arg;
		s->outbox.push_back(std::move(pc));
		s->cv.notify_all();      // ロングポーリング中の poll を即座に起こす
	}

	bool done;
	{
		std::unique_lock<std::mutex> lk(waiter->mu);
		done = waiter->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
		                           [&] { return waiter->done; });
	}

	if (!done) {
		sessions_.takeWaiter(id);   // 遅れて届いた結果は捨てられる
		out.set("ok", Json(false));
		out.set("error", Json("timed out after " + std::to_string(timeoutMs) + "ms"));
		return out;
	}

	std::lock_guard<std::mutex> lk(waiter->mu);
	out.set("ok", Json(waiter->ok));
	out.set("value", waiter->value);
	if (!waiter->error.empty()) out.set("error", Json(waiter->error));
	return out;
}

//---------------------------------------------------------------------------
void BrowserChannel::deliverResult(uint64_t id, bool ok, const Json& value,
                                   const std::string& error)
{
	auto w = sessions_.takeWaiter(id);
	if (!w) {
		logD("dropped result for unknown command id " + std::to_string(id));
		return;
	}
	std::lock_guard<std::mutex> lk(w->mu);
	w->ok    = ok;
	w->value = value;
	w->error = error;
	w->done  = true;
	w->cv.notify_all();
}

//---------------------------------------------------------------------------
void BrowserChannel::broadcast(const std::string& channel, const std::string& payload)
{
	sessions_.pushFrame(channel, sseFrame(payload));
}

void BrowserChannel::broadcastJson(const std::string& channel, const Json& payload)
{
	broadcast(channel, payload.dump());
}

} // namespace appserve
