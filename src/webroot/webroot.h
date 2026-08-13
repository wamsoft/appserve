//---------------------------------------------------------------------------
// WebRoot — UI アセットの供給元
//
// 優先順位付きで解決する:
//   1. --web-root=DIR          明示指定
//   2. カレントディレクトリの web/    ← 開発中はこれが勝つ
//   3. 実行ファイル隣の web/          インストール後の差し替え
//   4. 実行ファイル隣の <exe名>.zip / web.zip
//   5. 実行ファイルに埋め込まれた zip  ← リリース時の単一 exe
//
// これにより「開発中は web/ を見て動く / リリース時は zip を exe にくっつける」
// という要件が、どちらも同じコードパスで満たされる。
//---------------------------------------------------------------------------
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace appserve {

class WebRoot {
public:
	enum class Kind { None, Dir, Zip, Embedded };

	/// 解決を行う。explicitDir が非空ならそれだけを試す。
	/// 見つからなくても false を返すだけで、サーバは起動できる (API のみ提供)。
	bool resolve(const std::string& explicitDir);

	Kind               kind() const { return kind_; }
	const char*        kindName() const;
	/// 解決結果の場所 (ディレクトリパス / zip パス / "<embedded>")
	const std::string& location() const { return location_; }
	bool               valid() const { return kind_ != Kind::None; }

	/// アーカイブ/ディレクトリ内の相対パスを読む ("index.html", "lib/appserve.js")。
	/// 見つからなければ false。
	bool read(const std::string& rel, std::string& out) const;
	bool exists(const std::string& rel) const;

	/// 収録されているパス一覧 (Dir の場合は再帰列挙)。デバッグ/REPL 用。
	std::vector<std::string> list() const;

	/// 相対パスが安全か ("..", 絶対パス, ドライブ指定を拒否)
	static bool safeRelPath(const std::string& rel);

	static bool hasEmbedded();

private:
	struct Impl;
	Kind                  kind_ = Kind::None;
	std::string           location_;
	std::shared_ptr<Impl> impl_;
};

//---------------------------------------------------------------------------
/// 埋め込みアーカイブを登録する。appserve_embed_web() が生成する .cpp が
/// 静的初期化で呼ぶ。データは静的領域なのでコピーしない。
///
/// 生成コードは appserve のヘッダを一切 include せずに済むよう、この関数だけを
/// 自前で extern 宣言する。したがってシグネチャを変えてはいけない
/// (cmake/bin2c.cmake の宣言と対で管理すること)。
void appserveRegisterEmbeddedWeb(const unsigned char* data, size_t size);

} // namespace appserve
