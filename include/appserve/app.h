//---------------------------------------------------------------------------
// App — appserve アプリケーション本体
//
//   int main(int argc, char** argv) {
//       appserve::App app;
//       if (!app.parseArgs(argc, argv)) return app.exitCode();
//       app.options().appName = "My Tool";
//       app.addModule(appserve::makeFsModule());
//       app.addModule(std::make_unique<MyModule>());
//       return app.run();
//   }
//---------------------------------------------------------------------------
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "appserve/http.h"
#include "appserve/module.h"
#include "appserve/browser.h"
#include "appserve/log.h"

namespace appserve {

//---------------------------------------------------------------------------
struct Options {
	// --- アプリ識別 ---
	std::string appName    = "appserve";
	std::string appVersion = "0.1.0";

	// --- ネットワーク ---
	std::string host = "127.0.0.1";
	int         port = 0;            ///< 0 = OS に空きポートを割り当てさせる

	// --- UI ---
	std::string webRoot;             ///< --web-root。空なら自動解決 (webroot.cpp)
	bool        spa = false;         ///< 未知パスを index.html にフォールバック

	// --- ブラウザ起動 ---
	enum class BrowserMode { App, Default, None };
	BrowserMode              browser = BrowserMode::App;
	std::vector<std::string> browserArgs;

	// --- ライフサイクル (秒。0 で無効) ---
	int idleTimeout  = 10;
	int startupGrace = 30;
	int sessionTtl   = 20;

	// --- REPL ---
	bool        repl = false;        ///< stdin 対話 REPL
	std::string replFile;            ///< ファイルチャネルのディレクトリ

	// --- セキュリティ ---
	std::string token;               ///< 空なら起動時に自動生成
	bool        useToken = true;

	// --- ファイルアクセス ---
	std::vector<std::string> roots;  ///< 許可ルート (空なら無制限)
	bool                     allowWrite = false;

	// --- その他 ---
	std::string pluginDir;           ///< 空なら実行ファイル隣の plugins/
	bool        loadPlugins = true;
	LogLevel    logLevel = LogLevel::Info;
	std::string logFile;

	/// 派生アプリが自由に読む位置引数 (オプション以外の argv)
	std::vector<std::string> args;
};

//---------------------------------------------------------------------------
/// 独自コマンドラインオプションの記述
struct OptionSpec {
	std::string name;          ///< "--" 無しの名前 ("open")
	std::string valueHint;     ///< 空なら値なしフラグ ("PATH" 等)
	std::string help;
	std::function<bool(const std::string& value)> apply;   ///< false でエラー
};

//---------------------------------------------------------------------------
class App {
public:
	App();
	~App();
	App(const App&) = delete;
	App& operator=(const App&) = delete;

	Options&       options()       { return opt_; }
	const Options& options() const { return opt_; }

	/// 派生アプリの独自オプションを追加する (parseArgs より前に呼ぶこと)
	void addOption(OptionSpec spec);

	/// コマンドライン解析。false = 起動しない (--help / --version / 引数エラー)。
	/// 戻り値が false のときの終了コードは exitCode()。
	bool parseArgs(int argc, char** argv);
	int  exitCode() const { return exitCode_; }

	void addModule(std::unique_ptr<IModule> module);
	ApiRegistry&    registry();
	BrowserChannel& browser();

	/// サーバを起動し、終了までブロックする。戻り値がプロセス終了コード。
	int run();

	// --- ホストのメインループへ組み込む形 (run() の 3 分割) ---
	//
	// SDL のように自前のループを手放せないアプリ向け。run() と同じ順序で
	// 動くので、挙動の差はループを誰が回すかだけ:
	//
	//     if (!app.startServer()) return 1;
	//     while (app.serving()) { ...1 フレーム描画...; app.pump(0); }
	//     return app.stopServer();
	//
	// pump() は Affinity::Main のハンドラを**呼んだスレッドで**実行するので、
	// startServer() と同じスレッド (ホストのメインスレッド) から回すこと。

	/// サーバ起動 (モジュール登録 → listen → REPL → ブラウザ → onStart)。
	/// false = 起動失敗。終了コードは exitCode()。
	bool startServer();
	/// メインループ 1 周分 (タスク実行 + セッション回収 + 自動終了判定)。
	/// waitMs = キューが空のときの待ち時間。組込み時は 0。
	void pump(int waitMs = 0);
	/// サーバが動作中か (requestShutdown / 自動終了で false になる)
	bool serving() const;
	/// 終了処理。戻り値がプロセス終了コード。startServer() 前なら何もしない。
	int  stopServer();

	/// 終了を要求する (任意スレッドから可)
	void requestShutdown(int code = 0);

	// --- 稼働情報 ---
	bool        active() const;
	int         port() const;            ///< 実際に listen しているポート
	std::string url() const;             ///< "http://127.0.0.1:PORT/"
	std::string urlWithToken() const;    ///< ブラウザに渡す URL (?t=...)
	const std::string& token() const;

	/// ルータを直接叩く (REPL の .get / .post 用。HTTP を経由しない)
	Response dispatchInternal(const std::string& method, const std::string& path,
	                          const std::string& query = std::string(),
	                          const std::string& body  = std::string());

	/// ブラウザを開き直す
	bool openBrowser(const std::string& url = std::string());

	/// 内部実装へのアクセス (モジュール/REPL 用)
	struct Impl;
	Impl& impl() { return *impl_; }

private:
	Options                  opt_;
	int                      exitCode_ = 0;
	std::unique_ptr<Impl>    impl_;
};

} // namespace appserve
