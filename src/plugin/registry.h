//---------------------------------------------------------------------------
// Registry — ルート / 静的マウント / REPL コマンドの登録表
//
// 接続スレッドがマッチングのため読むので mutex で保護する。ハンドラ本体は
// shared_ptr で共有し、ロック下でポインタだけコピーして外で呼ぶ。
//---------------------------------------------------------------------------
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "appserve/module.h"

namespace appserve {

class App;

struct Route {
	std::string prefix;
	Affinity    affinity = Affinity::Main;
	Handler     handler;
	std::string owner;      ///< 登録元モジュール名 (.routes 表示用)
};

struct DirMount {
	std::string prefix;
	std::string dir;
	std::string owner;
};

struct ReplCmd {
	std::string name;       ///< "." 無し
	std::string help;
	ReplFn      fn;
	Affinity    affinity = Affinity::Main;
	std::string owner;
};

//---------------------------------------------------------------------------
class Registry : public ApiRegistry {
public:
	explicit Registry(App& app) : app_(app) {}

	// --- ApiRegistry ---
	void route(std::string prefix, Affinity affinity, Handler handler) override;
	bool unroute(const std::string& prefix) override;
	void mountDir(std::string prefix, std::string dir) override;
	bool unmountDir(const std::string& prefix) override;
	void replCommand(std::string name, std::string help, ReplFn fn,
	                 Affinity affinity) override;
	App& app() override { return app_; }

	// --- 検索 (接続スレッドから呼ばれる) ---
	/// パスに最長一致するルートを返す (無ければ nullptr)
	std::shared_ptr<const Route>    matchRoute(const std::string& path) const;
	std::shared_ptr<const DirMount> matchMount(const std::string& path) const;

	std::vector<std::shared_ptr<const Route>>    routes() const;
	std::vector<std::shared_ptr<const DirMount>> mounts() const;
	std::vector<std::shared_ptr<const ReplCmd>>  replCommands() const;
	std::shared_ptr<const ReplCmd>               findReplCommand(const std::string& name) const;

	/// 以降の登録に付ける owner 名 (モジュール登録中に設定する)
	void setOwner(std::string owner);

	/// 全登録を解放する (終了時。ハンドラが握るクロージャを手放す)
	void clear();

private:
	App&               app_;
	mutable std::mutex mu_;
	std::string        owner_ = "app";
	std::vector<std::shared_ptr<Route>>    routes_;
	std::vector<std::shared_ptr<DirMount>> mounts_;
	std::vector<std::shared_ptr<ReplCmd>>  repl_;
};

} // namespace appserve
