//---------------------------------------------------------------------------
// appserve ベースアプリ
//
// フレームワークそのものの動作見本であり、同時に「ローカルファイルブラウザ」
// として単体でも使える。派生アプリはこの main.cpp を写して、addModule に
// 自前のモジュールを足すところから始める。
//---------------------------------------------------------------------------
#include <appserve/appserve.h>

#include <cstdio>
#include <memory>

int main(int argc, char** argv)
{
	appserve::App app;

	app.options().appName    = "appserve";
	app.options().appVersion = APPSERVE_VERSION_STRING;

	// 独自オプションの例: 起動時に開くパスを UI へ渡す
	std::string openPath;
	app.addOption({
		"open", "PATH", "select this path in the UI on startup",
		[&openPath](const std::string& v) { openPath = v; return true; }
	});

	if (!app.parseArgs(argc, argv)) return app.exitCode();

	// --open が無ければ最初の位置引数を使う (appserve D:/work のように書ける)
	if (openPath.empty() && !app.options().args.empty())
		openPath = app.options().args.front();

	// 標準モジュール: ローカルファイルアクセス API
	app.addModule(appserve::makeFsModule());

	// 起動時パスを UI が読めるようにする小さな API
	app.registry().route("/api/app/startup", appserve::Affinity::Any,
		[&openPath](const appserve::Request&) {
			appserve::Json j = appserve::Json::object();
			j.set("open", appserve::Json(openPath));
			return appserve::Response::json(j);
		});

	return app.run();
}
