//---------------------------------------------------------------------------
// WebRoot 実装
//---------------------------------------------------------------------------
#include "webroot/webroot.h"
#include "webroot/zip.h"
#include "core/util.h"
#include "appserve/log.h"

#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace appserve {

namespace {

const unsigned char* g_embedded_data = nullptr;
size_t               g_embedded_size = 0;

/// ディレクトリ配信での最大ファイルサイズ (UI アセット想定の安全上限)
constexpr uint64_t kMaxAssetSize = 256ull * 1024 * 1024;

} // anonymous

//---------------------------------------------------------------------------
struct WebRoot::Impl {
	// Kind::Dir のとき
	std::string dir;
	// Kind::Zip / Embedded のとき
	std::string  zipBuffer;   // ファイルから読んだ場合の実体 (embedded では空)
	zip::Reader  reader;
	std::mutex   mu;          // reader は複数の接続スレッドから読まれる
};

//---------------------------------------------------------------------------
void appserveRegisterEmbeddedWeb(const unsigned char* data, size_t size)
{
	g_embedded_data = data;
	g_embedded_size = size;
}

bool WebRoot::hasEmbedded()
{
	return g_embedded_data != nullptr && g_embedded_size > 0;
}

const char* WebRoot::kindName() const
{
	switch (kind_) {
		case Kind::Dir:      return "dir";
		case Kind::Zip:      return "zip";
		case Kind::Embedded: return "embedded";
		default:             return "none";
	}
}

//---------------------------------------------------------------------------
bool WebRoot::safeRelPath(const std::string& rel)
{
	if (rel.empty()) return false;
	if (rel.front() == '/' || rel.front() == '\\') return false;
	if (rel.find('\\') != std::string::npos) return false;
	if (rel.find(':')  != std::string::npos) return false;
	// ".." セグメントを拒否 (吉里吉里Z の PathIsUnsafe と同じ方針)
	size_t start = 0;
	while (start <= rel.size()) {
		size_t sl = rel.find('/', start);
		std::string seg = (sl == std::string::npos)
			? rel.substr(start) : rel.substr(start, sl - start);
		if (seg == "..") return false;
		if (sl == std::string::npos) break;
		start = sl + 1;
	}
	return true;
}

//---------------------------------------------------------------------------
bool WebRoot::resolve(const std::string& explicitDir)
{
	kind_ = Kind::None;
	location_.clear();
	impl_ = std::make_shared<Impl>();

	auto tryDir = [&](const std::string& d) -> bool {
		if (d.empty() || !util::isDirectory(d)) return false;
		impl_->dir = util::normalizeSlash(d);
		kind_      = Kind::Dir;
		location_  = impl_->dir;
		return true;
	};
	auto tryZip = [&](const std::string& path) -> bool {
		if (path.empty() || !util::isFile(path)) return false;
		std::string data;
		if (!util::readFile(path, data)) return false;
		impl_->zipBuffer = std::move(data);
		if (!impl_->reader.open((const unsigned char*)impl_->zipBuffer.data(),
		                        impl_->zipBuffer.size())) {
			logW("WebRoot: " + path + " is not a readable zip (" +
			     impl_->reader.error() + ")");
			impl_->zipBuffer.clear();
			return false;
		}
		kind_     = Kind::Zip;
		location_ = util::normalizeSlash(path);
		return true;
	};

	// 1. 明示指定 (ディレクトリでも zip でも受ける)
	if (!explicitDir.empty()) {
		if (tryDir(explicitDir) || tryZip(explicitDir)) return true;
		logW("WebRoot: --web-root '" + explicitDir + "' not found");
		return false;
	}

	// 2. カレントの web/
	if (tryDir(util::joinPath(util::currentDir(), "web"))) return true;

	// 3-4. 実行ファイル隣
	std::string exeDir = util::executableDir();
	if (!exeDir.empty()) {
		if (tryDir(util::joinPath(exeDir, "web"))) return true;

		std::string exe = util::executablePath();
		std::string stem;
		{
			size_t sl  = exe.find_last_of('/');
			std::string base = (sl == std::string::npos) ? exe : exe.substr(sl + 1);
			size_t dot = base.rfind('.');
			stem = (dot == std::string::npos) ? base : base.substr(0, dot);
		}
		if (!stem.empty() && tryZip(util::joinPath(exeDir, stem + ".zip"))) return true;
		if (tryZip(util::joinPath(exeDir, "web.zip"))) return true;
	}

	// 5. 埋め込み
	if (hasEmbedded()) {
		if (impl_->reader.open(g_embedded_data, g_embedded_size)) {
			kind_     = Kind::Embedded;
			location_ = "<embedded>";
			return true;
		}
		logW("WebRoot: embedded archive is unreadable (" + impl_->reader.error() + ")");
	}

	return false;
}

//---------------------------------------------------------------------------
bool WebRoot::read(const std::string& rel, std::string& out) const
{
	if (!impl_ || !safeRelPath(rel)) return false;

	if (kind_ == Kind::Dir) {
		std::string full = util::joinPath(impl_->dir, rel);
		return util::readFile(full, out, kMaxAssetSize);
	}
	if (kind_ == Kind::Zip || kind_ == Kind::Embedded) {
		std::lock_guard<std::mutex> lk(impl_->mu);
		if (!impl_->reader.extract(rel, out)) return false;
		return true;
	}
	return false;
}

bool WebRoot::exists(const std::string& rel) const
{
	if (!impl_ || !safeRelPath(rel)) return false;
	if (kind_ == Kind::Dir) return util::isFile(util::joinPath(impl_->dir, rel));
	if (kind_ == Kind::Zip || kind_ == Kind::Embedded) {
		std::lock_guard<std::mutex> lk(impl_->mu);
		return impl_->reader.find(rel) != nullptr;
	}
	return false;
}

//---------------------------------------------------------------------------
std::vector<std::string> WebRoot::list() const
{
	std::vector<std::string> out;
	if (!impl_) return out;

	if (kind_ == Kind::Dir) {
		std::error_code ec;
		fs::path base = fs::u8path(impl_->dir);
		for (auto it = fs::recursive_directory_iterator(base, ec);
		     it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) break;
			if (!it->is_regular_file(ec)) continue;
			std::string rel = fs::relative(it->path(), base, ec).u8string();
			if (ec) continue;
			out.push_back(util::normalizeSlash(rel));
		}
	} else if (kind_ == Kind::Zip || kind_ == Kind::Embedded) {
		std::lock_guard<std::mutex> lk(impl_->mu);
		for (const auto& e : impl_->reader.entries()) out.push_back(e.name);
	}
	return out;
}

} // namespace appserve
