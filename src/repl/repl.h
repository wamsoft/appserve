//---------------------------------------------------------------------------
// Repl — サーバ状態確認 + ブラウザ遠隔操作のコマンドシェル
//
// TJS のようなスクリプトエンジンは持たないので、サーバ側はドットコマンド式。
// 任意コードの評価が要る場面は「ブラウザ側で JS を評価する」(.b eval) 経路へ
// 寄せている (UI ロジックはすべて JS 側にあるため、実用上これで足りる)。
//
// チャネルは 3 系統で、いずれも同じ execute() を通る:
//   stdin       --repl         人間の対話
//   ファイル    --replfile=DIR AI エージェント / CI (吉里吉里Z と同一プロトコル)
//   HTTP        POST /_app/repl  curl / ブラウザ内コンソール
//---------------------------------------------------------------------------
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace appserve {

class App;
class Registry;

class Repl {
public:
	struct Result {
		bool        ok = true;
		std::string text;
		std::string error;
	};

	Repl(App& app, Registry& registry);
	~Repl();

	/// 1 行を実行する。任意スレッドから呼べる (Affinity に従いメインへ運ぶ)。
	Result execute(const std::string& line);

	/// stdin 対話チャネルを開始する
	void startStdin();
	/// ファイルチャネルを開始する (dir 配下に cmd / resp を置く)
	void startFileChannel(const std::string& dir);
	/// 全チャネルを停止する
	void stop();

	/// 対話/ファイルチャネルがアタッチされているか。
	/// アタッチ中はアイドル自動終了を抑止する (開発中にブラウザを閉じても
	/// サーバが落ちないようにするため)。
	bool attached() const;

	std::string helpText() const;

private:
	void stdinLoop();
	void fileLoop(std::string dir);

	App&              app_;
	Registry&         registry_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stdin_on_{false};
	std::atomic<bool> file_on_{false};
	std::thread       stdin_thread_;
	std::thread       file_thread_;
};

/// 内蔵コマンド (.help / .info / .routes / .b ...) を登録する
void registerBuiltinReplCommands(App& app, Registry& registry);

} // namespace appserve
