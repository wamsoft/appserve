//---------------------------------------------------------------------------
// モジュール / API レジストリ
//
// 派生アプリは IModule を実装して App::addModule() で足す。
// 動的プラグイン (DLL) も最終的に同じ ApiRegistry へ登録される。
//---------------------------------------------------------------------------
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "appserve/http.h"

namespace appserve {

class App;

/// REPL コマンドの実装。args は行の残り部分 (空文字あり)。
/// 戻り値は REPL へ表示する文字列。エラーは std::runtime_error を投げる。
using ReplFn = std::function<std::string(const std::string& args)>;

//---------------------------------------------------------------------------
class ApiRegistry {
public:
	virtual ~ApiRegistry() = default;

	/// パスプレフィックス最長一致でハンドラを登録する。prefix は "/" 始まり。
	/// 同一 prefix の再登録は上書き。
	virtual void route(std::string prefix, Affinity affinity, Handler handler) = 0;
	/// Affinity::Main での登録 (既定)
	void route(std::string prefix, Handler handler) {
		route(std::move(prefix), Affinity::Main, std::move(handler));
	}
	virtual bool unroute(const std::string& prefix) = 0;

	/// prefix 以下の GET をローカルディレクトリから配信する ("../" は 403)
	virtual void mountDir(std::string prefix, std::string dir) = 0;
	virtual bool unmountDir(const std::string& prefix) = 0;

	/// REPL のドットコマンドを追加する。name は "." を含まない ("psd" → ".psd")
	virtual void replCommand(std::string name, std::string help, ReplFn fn,
	                         Affinity affinity = Affinity::Main) = 0;

	/// 所属アプリ (ブラウザ制御やオプション参照に使う)
	virtual App& app() = 0;
};

//---------------------------------------------------------------------------
class IModule {
public:
	virtual ~IModule() = default;
	/// モジュール名 (ログ / .routes 表示用)
	virtual const char* name() const = 0;
	/// API 登録。サーバ起動前にメインスレッドで 1 回呼ばれる。
	virtual void registerApi(ApiRegistry& reg) = 0;
	/// 起動完了後 (listen 済み・ブラウザ起動後) に呼ばれる
	virtual void onStart(App& app) {}
	/// 終了時にメインスレッドで呼ばれる。リソース解放はここで。
	virtual void onShutdown() {}
};

//---------------------------------------------------------------------------
/// 標準モジュール: ローカルファイルアクセス API (/api/fs/*)
std::unique_ptr<IModule> makeFsModule();

} // namespace appserve
