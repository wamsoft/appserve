//---------------------------------------------------------------------------
// stdin 対話チャネル
//
// 行編集ライブラリは持たない (依存ゼロ方針)。人間が使う場合は端末側の
// 行編集で足りるし、エージェントはファイルチャネル / HTTP を使う。
//---------------------------------------------------------------------------
#include "repl/repl.h"
#include "appserve/app.h"
#include "appserve/log.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace appserve {

void Repl::startStdin()
{
	if (stdin_on_.exchange(true)) return;
	stdin_thread_ = std::thread(&Repl::stdinLoop, this);
}

void Repl::stdinLoop()
{
	logI("repl: type .help for commands");
	std::string line;
	while (running_.load(std::memory_order_acquire)) {
		std::fputs("appserve> ", stdout);
		std::fflush(stdout);

		if (!std::getline(std::cin, line)) {
			// EOF (Ctrl+D / パイプ終端)。対話が終わったのでサーバも畳む。
			std::fputs("\n", stdout);
			if (running_.load(std::memory_order_acquire)) app_.requestShutdown(0);
			break;
		}
		if (!running_.load(std::memory_order_acquire)) break;

		Result r = execute(line);
		if (!r.ok) {
			std::fprintf(stdout, "error: %s\n", r.error.c_str());
		} else if (!r.text.empty()) {
			std::fputs(r.text.c_str(), stdout);
			if (r.text.back() != '\n') std::fputc('\n', stdout);
		}
		std::fflush(stdout);
	}
	stdin_on_.store(false);
}

} // namespace appserve
