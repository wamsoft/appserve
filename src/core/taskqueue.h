//---------------------------------------------------------------------------
// TaskQueue — 接続スレッド → メインスレッド のタスク受け渡し
//
// Affinity::Main のハンドラは、接続スレッドがここへ submit し、メインループが
// drain して実行する。submit は実行完了までブロックするので、呼び出し側は
// あたかも同期呼び出しのように書ける (吉里吉里Z の ReplMainQueue と同じモデル)。
//---------------------------------------------------------------------------
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace appserve {

class TaskQueue {
public:
	/// fn をメインスレッドで実行し、完了まで待つ。
	/// 戻り値: 実行できたら true。停止中 (shutdown 済み) なら false。
	/// メインスレッド自身から呼ばれた場合はその場で実行する (自己デッドロック回避)。
	bool submit(std::function<void()> fn);

	/// キューに積むだけで待たない (結果を使わない通知用)
	void post(std::function<void()> fn);

	/// メインスレッドから呼ぶ。待ちタスクを最大 maxTasks 件実行する。
	/// キューが空なら最大 waitMs ミリ秒だけ待つ。実行した件数を返す。
	int drain(int waitMs, int maxTasks = 64);

	/// 以降の submit を拒否し、待機中のものを解放する
	void shutdown();
	bool isShutdown() const { return shutdown_.load(std::memory_order_acquire); }

	/// 現在のスレッドをメインスレッドとして登録する (run() の先頭で呼ぶ)
	void bindMainThread();
	bool onMainThread() const;

	/// 未処理タスク数
	size_t pending() const;

private:
	struct Task {
		std::function<void()>   fn;
		bool                    wait = false;
		bool                    done = false;
		std::mutex              mu;
		std::condition_variable cv;
	};

	mutable std::mutex               mu_;
	std::condition_variable          cv_;
	std::deque<std::shared_ptr<Task>> queue_;
	std::atomic<bool>                shutdown_{false};
	std::atomic<unsigned long long>  main_thread_{0};
};

} // namespace appserve
