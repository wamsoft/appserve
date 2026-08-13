//---------------------------------------------------------------------------
// SessionManager 実装
//---------------------------------------------------------------------------
#include "core/session.h"
#include "core/util.h"

#include <algorithm>

namespace appserve {

//---------------------------------------------------------------------------
std::string sseFrame(const std::string& payload)
{
	// SSE は 1 行 = "data: <text>" で、空行がイベント終端。
	// payload に改行が含まれる場合は複数の data 行に分解する。
	std::string frame;
	size_t start = 0;
	while (true) {
		size_t nl = payload.find('\n', start);
		std::string line = (nl == std::string::npos)
			? payload.substr(start) : payload.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		frame += "data: ";
		frame += line;
		frame += "\n";
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	frame += "\n";
	return frame;
}

//---------------------------------------------------------------------------
// セッション
//---------------------------------------------------------------------------
std::shared_ptr<Session> SessionManager::create(const std::string& ua,
                                                const std::string& remote)
{
	auto s = std::make_shared<Session>();
	s->sid        = util::randomHex(12);
	s->ua         = ua;
	s->remote     = remote;
	s->createdMs  = util::nowMs();
	s->lastSeenMs = s->createdMs;
	std::lock_guard<std::mutex> lk(mu_);
	sessions_.push_back(s);
	return s;
}

std::shared_ptr<Session> SessionManager::find(const std::string& sid)
{
	if (sid.empty()) return nullptr;
	std::lock_guard<std::mutex> lk(mu_);
	for (auto& s : sessions_) if (s->sid == sid) return s;
	return nullptr;
}

std::shared_ptr<Session> SessionManager::newest()
{
	std::lock_guard<std::mutex> lk(mu_);
	std::shared_ptr<Session> best;
	int64_t bestSeen = -1;
	for (auto& s : sessions_) {
		int64_t seen;
		{
			std::lock_guard<std::mutex> slk(s->mu);
			if (s->closed) continue;
			seen = s->lastSeenMs;
		}
		if (seen > bestSeen) { bestSeen = seen; best = s; }
	}
	return best;
}

void SessionManager::touch(const std::string& sid)
{
	auto s = find(sid);
	if (!s) return;
	std::lock_guard<std::mutex> lk(s->mu);
	s->lastSeenMs = util::nowMs();
}

void SessionManager::remove(const std::string& sid)
{
	std::shared_ptr<Session> victim;
	{
		std::lock_guard<std::mutex> lk(mu_);
		for (size_t i = 0; i < sessions_.size(); ++i) {
			if (sessions_[i]->sid == sid) {
				victim = sessions_[i];
				sessions_.erase(sessions_.begin() + (long)i);
				break;
			}
		}
	}
	if (!victim) return;
	// ロングポーリング中のスレッドを起こす
	std::lock_guard<std::mutex> lk(victim->mu);
	victim->closed = true;
	victim->cv.notify_all();
}

size_t SessionManager::reap(int ttlSec)
{
	if (ttlSec <= 0) return count();
	int64_t deadline = util::nowMs() - (int64_t)ttlSec * 1000;
	std::vector<std::shared_ptr<Session>> dead;
	size_t alive = 0;
	{
		std::lock_guard<std::mutex> lk(mu_);
		for (size_t i = 0; i < sessions_.size();) {
			auto& s = sessions_[i];
			int64_t seen;
			{
				std::lock_guard<std::mutex> slk(s->mu);
				seen = s->lastSeenMs;
			}
			if (seen < deadline) {
				dead.push_back(s);
				sessions_.erase(sessions_.begin() + (long)i);
			} else {
				++alive;
				++i;
			}
		}
	}
	for (auto& s : dead) {
		std::lock_guard<std::mutex> lk(s->mu);
		s->closed = true;
		s->cv.notify_all();
	}
	return alive;
}

size_t SessionManager::count()
{
	std::lock_guard<std::mutex> lk(mu_);
	return sessions_.size();
}

std::vector<std::shared_ptr<Session>> SessionManager::all()
{
	std::lock_guard<std::mutex> lk(mu_);
	return sessions_;
}

void SessionManager::closeAll()
{
	std::vector<std::shared_ptr<Session>> list;
	{
		std::lock_guard<std::mutex> lk(mu_);
		list.swap(sessions_);
	}
	for (auto& s : list) {
		std::lock_guard<std::mutex> lk(s->mu);
		s->closed = true;
		s->cv.notify_all();
	}
}

//---------------------------------------------------------------------------
// コマンド往復
//---------------------------------------------------------------------------
void SessionManager::addWaiter(uint64_t id, std::shared_ptr<CallWaiter> w)
{
	std::lock_guard<std::mutex> lk(waiters_mu_);
	waiters_.emplace_back(id, std::move(w));
}

std::shared_ptr<CallWaiter> SessionManager::takeWaiter(uint64_t id)
{
	std::lock_guard<std::mutex> lk(waiters_mu_);
	for (size_t i = 0; i < waiters_.size(); ++i) {
		if (waiters_[i].first == id) {
			auto w = waiters_[i].second;
			waiters_.erase(waiters_.begin() + (long)i);
			return w;
		}
	}
	return nullptr;
}

size_t SessionManager::waiterCount()
{
	std::lock_guard<std::mutex> lk(waiters_mu_);
	return waiters_.size();
}

//---------------------------------------------------------------------------
// SSE
//---------------------------------------------------------------------------
std::shared_ptr<SseClient> SessionManager::addSse(const std::string& channel)
{
	auto c = std::make_shared<SseClient>();
	c->channel = channel;
	std::lock_guard<std::mutex> lk(sse_mu_);
	sse_.push_back(c);
	return c;
}

void SessionManager::removeSse(const std::shared_ptr<SseClient>& c)
{
	std::lock_guard<std::mutex> lk(sse_mu_);
	for (size_t i = 0; i < sse_.size(); ++i) {
		if (sse_[i] == c) { sse_.erase(sse_.begin() + (long)i); return; }
	}
}

void SessionManager::pushFrame(const std::string& channel, const std::string& frame)
{
	std::lock_guard<std::mutex> lk(sse_mu_);
	for (auto& c : sse_) {
		if (c->channel != channel) continue;
		std::lock_guard<std::mutex> clk(c->mu);
		c->queue.push_back(frame);
		c->cv.notify_one();
	}
}

void SessionManager::closeAllSse()
{
	std::lock_guard<std::mutex> lk(sse_mu_);
	for (auto& c : sse_) {
		std::lock_guard<std::mutex> clk(c->mu);
		c->closed = true;
		c->cv.notify_all();
	}
}

size_t SessionManager::sseCount()
{
	std::lock_guard<std::mutex> lk(sse_mu_);
	return sse_.size();
}

//---------------------------------------------------------------------------
// ログバックログ
//---------------------------------------------------------------------------
void SessionManager::pushLog(const std::string& json)
{
	std::lock_guard<std::mutex> lk(ring_mu_);
	ring_.push_back(json);
	while (ring_.size() > kRingMax) ring_.pop_front();
}

std::vector<std::string> SessionManager::logBacklog()
{
	std::lock_guard<std::mutex> lk(ring_mu_);
	return std::vector<std::string>(ring_.begin(), ring_.end());
}

} // namespace appserve
