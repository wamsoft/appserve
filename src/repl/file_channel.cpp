//---------------------------------------------------------------------------
// ファイルチャネル REPL (--replfile=DIR)
//
// console を介さずにエージェントが REPL を駆動するための、ファイルベースの
// lockstep プロトコル。吉里吉里Z の -replfile と同一仕様なので、エージェント
// 側の手順をそのまま流用できる。
//
//   1. エージェント: コマンド (UTF-8) を cmd.tmp に書き、cmd に rename
//   2. サーバ:       cmd を検出 → 読取 → 削除 → 実行 → 結果 JSON を
//                    resp.tmp に書き resp に rename
//   3. エージェント: resp の出現を待ち、読取 → 削除。次コマンドへ
//
// 未読の resp が残る間は次コマンドを処理しない (取りこぼし防止)。
//---------------------------------------------------------------------------
#include "repl/repl.h"
#include "appserve/app.h"
#include "appserve/json.h"
#include "appserve/log.h"
#include "core/util.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace appserve {

void Repl::startFileChannel(const std::string& dir)
{
	if (dir.empty()) return;
	if (file_on_.exchange(true)) return;
	file_thread_ = std::thread(&Repl::fileLoop, this, dir);
}

void Repl::fileLoop(std::string dir)
{
	std::error_code ec;
	fs::create_directories(fs::u8path(dir), ec);
	if (!util::isDirectory(dir)) {
		logE("replfile: cannot use directory " + dir);
		file_on_.store(false);
		return;
	}

	const std::string cmdPath     = util::joinPath(dir, "cmd");
	const std::string cmdTmpPath  = util::joinPath(dir, "cmd.tmp");
	const std::string respPath    = util::joinPath(dir, "resp");
	const std::string respTmpPath = util::joinPath(dir, "resp.tmp");

	// 起動時に前回の残骸を掃除する
	fs::remove(fs::u8path(cmdPath), ec);
	fs::remove(fs::u8path(cmdTmpPath), ec);
	fs::remove(fs::u8path(respPath), ec);
	fs::remove(fs::u8path(respTmpPath), ec);

	logI("replfile: channel ready at " + dir);

	while (running_.load(std::memory_order_acquire)) {
		// 前回の応答がまだ読まれていなければ待つ
		if (util::isFile(respPath)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}
		if (!util::isFile(cmdPath)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}

		std::string line;
		if (!util::readFile(cmdPath, line)) {
			// 書き込み途中で読んでしまった可能性。少し待って再試行する。
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		fs::remove(fs::u8path(cmdPath), ec);

		Result r = execute(line);

		Json j = Json::object();
		j.set("ok",     Json(r.ok));
		j.set("result", Json(r.text));
		j.set("error",  Json(r.error));
		std::string payload = j.dump();

		// tmp へ書いてから rename することで、部分書き込みを読まれないようにする
		if (util::writeFile(respTmpPath, payload)) {
			fs::rename(fs::u8path(respTmpPath), fs::u8path(respPath), ec);
			if (ec) {
				logW("replfile: rename failed: " + ec.message());
				fs::remove(fs::u8path(respTmpPath), ec);
			}
		} else {
			logW("replfile: could not write response");
		}
	}

	fs::remove(fs::u8path(respPath), ec);
	file_on_.store(false);
}

} // namespace appserve
