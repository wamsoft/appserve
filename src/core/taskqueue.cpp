//---------------------------------------------------------------------------
// TaskQueue 実装
//---------------------------------------------------------------------------
#include "core/taskqueue.h"
#include "appserve/log.h"

#include <chrono>
#include <thread>

namespace appserve {

namespace {
unsigned long long threadId()
{
	return (unsigned long long)std::hash<std::thread::id>{}(std::this_thread::get_id());
}
} // anonymous

//---------------------------------------------------------------------------
void TaskQueue::bindMainThread()
{
	main_thread_.store(threadId(), std::memory_order_release);
}

bool TaskQueue::onMainThread() const
{
	unsigned long long m = main_thread_.load(std::memory_order_acquire);
	return m != 0 && m == threadId();
}

size_t TaskQueue::pending() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return queue_.size();
}

//---------------------------------------------------------------------------
bool TaskQueue::submit(std::function<void()> fn)
{
	if (!fn) return true;
	if (shutdown_.load(std::memory_order_acquire)) return false;

	// メインスレッド自身からの submit はその場で実行する。
	// (REPL コマンドが Affinity::Main で走っている最中に dispatchInternal を
	//  呼ぶ、といった経路で自己デッドロックするのを防ぐ)
	if (onMainThread()) {
		fn();
		return true;
	}

	auto task = std::make_shared<Task>();
	task->fn   = std::move(fn);
	task->wait = true;
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (shutdown_.load(std::memory_order_acquire)) return false;
		queue_.push_back(task);
	}
	cv_.notify_one();

	std::unique_lock<std::mutex> lk(task->mu);
	task->cv.wait(lk, [&] { return task->done; });
	return true;
}

void TaskQueue::post(std::function<void()> fn)
{
	if (!fn) return;
	if (shutdown_.load(std::memory_order_acquire)) return;
	if (onMainThread()) { fn(); return; }

	auto task = std::make_shared<Task>();
	task->fn   = std::move(fn);
	task->wait = false;
	{
		std::lock_guard<std::mutex> lk(mu_);
		queue_.push_back(task);
	}
	cv_.notify_one();
}

//---------------------------------------------------------------------------
int TaskQueue::drain(int waitMs, int maxTasks)
{
	int executed = 0;
	while (executed < maxTasks) {
		std::shared_ptr<Task> task;
		{
			std::unique_lock<std::mutex> lk(mu_);
			if (queue_.empty()) {
				// 最初の 1 件だけ待つ。2 件目以降は空になった時点で抜ける。
				if (executed > 0 || waitMs <= 0) break;
				cv_.wait_for(lk, std::chrono::milliseconds(waitMs),
				             [&] { return !queue_.empty() ||
				                          shutdown_.load(std::memory_order_acquire); });
				if (queue_.empty()) break;
			}
			task = queue_.front();
			queue_.pop_front();
		}

		// ハンドラ例外がメインループを落とさないようにここで捕まえる。
		// (個々のハンドラでも捕捉しているが最後の砦)
		try {
			task->fn();
		} catch (const std::exception& e) {
			logE(std::string("task threw: ") + e.what());
		} catch (...) {
			logE("task threw unknown exception");
		}
		++executed;

		if (task->wait) {
			std::lock_guard<std::mutex> lk(task->mu);
			task->done = true;
			task->cv.notify_all();
		}
	}
	return executed;
}

//---------------------------------------------------------------------------
void TaskQueue::shutdown()
{
	std::deque<std::shared_ptr<Task>> left;
	{
		std::lock_guard<std::mutex> lk(mu_);
		shutdown_.store(true, std::memory_order_release);
		left.swap(queue_);
	}
	cv_.notify_all();
	// 待機中の submit を解放する (fn は実行しない)
	for (auto& t : left) {
		if (!t->wait) continue;
		std::lock_guard<std::mutex> lk(t->mu);
		t->done = true;
		t->cv.notify_all();
	}
}

} // namespace appserve
