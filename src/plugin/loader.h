//---------------------------------------------------------------------------
// PluginLoader — 動的プラグイン (DLL / so / dylib) のロード
//
// プラグインは C ABI (include/appserve/plugin_abi.h) の appserve_plugin_init
// だけを export する。C++ の型を境界で跨がせないので、appserve 側を再ビルド
// しなくてもプラグインを差し替えられる。
//---------------------------------------------------------------------------
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace appserve {

class App;
class Registry;

class PluginLoader {
public:
	PluginLoader(App& app, Registry& registry);
	~PluginLoader();

	/// dir 直下の共有ライブラリを列挙してロードする。dir が無ければ何もしない。
	/// ロードできた個数を返す。
	int  loadDirectory(const std::string& dir);
	/// 1 つだけロードする
	bool loadFile(const std::string& path);
	void unloadAll();

	struct Loaded {
		std::string path;
		void*       handle = nullptr;
	};
	const std::vector<Loaded>& loaded() const { return loaded_; }

private:
	App&                app_;
	Registry&           registry_;
	std::vector<Loaded> loaded_;
};

} // namespace appserve
