//---------------------------------------------------------------------------
// Repl 本体 (コマンド解釈 + チャネル管理)
//---------------------------------------------------------------------------
#include "repl/repl.h"
#include "core/app_impl.h"

#include "appserve/log.h"
#include "core/util.h"

#include <algorithm>
#include <stdexcept>

namespace appserve {

Repl::Repl(App& app, Registry& registry) : app_(app), registry_(registry)
{
	running_.store(true);
}

Repl::~Repl() { stop(); }

bool Repl::attached() const
{
	return stdin_on_.load(std::memory_order_relaxed) ||
	       file_on_.load(std::memory_order_relaxed);
}

//---------------------------------------------------------------------------
Repl::Result Repl::execute(const std::string& lineIn)
{
	Result r;
	std::string line = util::trim(lineIn);
	if (line.empty()) return r;

	// 先頭の "." は付けても付けなくても良い
	if (line[0] == '.') line.erase(0, 1);

	std::string rest;
	std::string name = util::nextToken(line, rest);
	if (name.empty()) return r;

	if (name == "quit" || name == "exit") {
		r.text = "shutting down";
		app_.requestShutdown(0);
		return r;
	}

	auto cmd = registry_.findReplCommand(name);
	if (!cmd) {
		r.ok    = false;
		r.error = "unknown command '." + name + "' (try .help)";
		return r;
	}

	auto invoke = [&] {
		try {
			r.text = cmd->fn(rest);
		} catch (const std::exception& e) {
			r.ok    = false;
			r.error = e.what();
		} catch (...) {
			r.ok    = false;
			r.error = "unknown error";
		}
	};

	if (cmd->affinity == Affinity::Any) {
		invoke();
	} else if (!app_.impl().queue.submit(invoke)) {
		r.ok    = false;
		r.error = "server is shutting down";
	}
	return r;
}

//---------------------------------------------------------------------------
std::string Repl::helpText() const
{
	auto cmds = registry_.replCommands();
	std::sort(cmds.begin(), cmds.end(),
	          [](const std::shared_ptr<const ReplCmd>& a,
	             const std::shared_ptr<const ReplCmd>& b) { return a->name < b->name; });

	std::string out = "commands:\n";
	for (const auto& c : cmds) {
		std::string left = "  ." + c->name;
		while (left.size() < 22) left += ' ';
		out += left + c->help + "\n";
	}
	out += "  .quit                 stop the server\n";
	return out;
}

//---------------------------------------------------------------------------
void Repl::stop()
{
	if (!running_.exchange(false)) return;
	// 各ループは running_ を見て自発的に抜ける。stdin は読み取りでブロック
	// したままになりうるので detach 相当の扱いにする (プロセス終了で解放)。
	if (file_thread_.joinable()) file_thread_.join();
	if (stdin_thread_.joinable()) stdin_thread_.detach();
	stdin_on_.store(false);
	file_on_.store(false);
}

} // namespace appserve
