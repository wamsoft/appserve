//---------------------------------------------------------------------------
// 標準モジュール: ローカルファイルアクセス (/api/fs/*)
//
// ベースアプリの主機能であると同時に、派生アプリ (psdparse アプリ等) の
// 「ファイル選択部品」としてそのまま再利用される。
//
// 安全策:
//   - --root=DIR が指定されていればその配下しか触れない (複数指定可)
//   - 書き込み系は --allow-write が無ければ 403
//   - 読み出しは既定 64MB 上限 (それ以上は download で range 取得させる)
//---------------------------------------------------------------------------
#include "appserve/appserve.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace appserve {

namespace {

constexpr uint64_t kDefaultReadLimit = 64ull * 1024 * 1024;

//---------------------------------------------------------------------------
std::string normalize(std::string p)
{
	for (char& c : p) if (c == '\\') c = '/';
	return p;
}

/// path を絶対パスに正規化する (シンボリックリンクは辿らない)
std::string absolutePath(const std::string& p)
{
	std::error_code ec;
	fs::path abs = fs::absolute(fs::u8path(p), ec);
	if (ec) return normalize(p);
	fs::path norm = abs.lexically_normal();
	return normalize(norm.u8string());
}

//---------------------------------------------------------------------------
/// file_time_type → Unix エポックのミリ秒。
///
/// file_time_type のエポックは実装依存 (Windows は 1601-01-01) で、C++17 には
/// 移植性のある変換が無い。両クロックの「今」を 1 度だけ突き合わせて差分を
/// 求め、それを補正値として使う。
int64_t fileTimeToUnixMs(fs::file_time_type t)
{
	using namespace std::chrono;
	static const int64_t offsetMs = [] {
		int64_t sysNow = duration_cast<milliseconds>(
			system_clock::now().time_since_epoch()).count();
		int64_t fileNow = duration_cast<milliseconds>(
			fs::file_time_type::clock::now().time_since_epoch()).count();
		return sysNow - fileNow;
	}();
	return duration_cast<milliseconds>(t.time_since_epoch()).count() + offsetMs;
}

//---------------------------------------------------------------------------
/// バッファ末尾の「途中で切れた UTF-8 シーケンス」を取り除く。
///
/// これをしないと、長いファイルを length 指定で部分読みしたときに末尾が
/// 不正な UTF-8 になり、UTF-8 のファイルを CP932 と誤判定してしまう。
void trimIncompleteUtf8Tail(std::string& s)
{
	// 継続バイト (10xxxxxx) を最大 3 つ遡り、先頭バイトを見て長さが足りるか判定する
	size_t i = s.size();
	size_t trailers = 0;
	while (i > 0 && trailers < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
		--i;
		++trailers;
	}
	if (i == 0) return;
	unsigned char lead = (unsigned char)s[i - 1];
	size_t need;
	if      (lead < 0x80)          need = 0;      // ASCII: 継続バイト不要
	else if ((lead & 0xE0) == 0xC0) need = 1;
	else if ((lead & 0xF0) == 0xE0) need = 2;
	else if ((lead & 0xF8) == 0xF0) need = 3;
	else return;                                   // 不正な先頭バイト。触らない
	if (need > trailers) s.resize(i - 1);          // 足りない = 途中で切れている
}

/// a が b の配下 (または同一) か
bool isUnder(const std::string& child, const std::string& parent)
{
	std::string c = child, p = parent;
#ifdef _WIN32
	// Windows のパスは大小文字を区別しない
	std::transform(c.begin(), c.end(), c.begin(),
	               [](unsigned char ch) { return (char)tolower(ch); });
	std::transform(p.begin(), p.end(), p.begin(),
	               [](unsigned char ch) { return (char)tolower(ch); });
#endif
	if (!p.empty() && p.back() == '/') p.pop_back();
	if (c == p) return true;
	return c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/';
}

//---------------------------------------------------------------------------
class FsModule : public IModule {
public:
	const char* name() const override { return "fs"; }

	void registerApi(ApiRegistry& reg) override {
		app_ = &reg.app();
		// prefix 最長一致で 1 本受けて、suffix で分岐する
		reg.route("/api/fs/", Affinity::Main,
		          [this](const Request& r) { return handle(r); });

		reg.replCommand("ls", "list a directory through the file API",
		                [this](const std::string& args) {
			std::string path = args.empty() ? std::string(".") : args;
			Json j = listDir(absolutePath(path));
			return j.dump(2) + "\n";
		});
	}

private:
	App* app_ = nullptr;

	const Options& opt() const { return app_->options(); }

	//-----------------------------------------------------------------------
	/// --root 制限のチェック。許可されていなければ理由を返す (空なら OK)。
	std::string checkAccess(const std::string& absPath) const {
		const auto& roots = opt().roots;
		if (roots.empty()) return std::string();
		for (const auto& r : roots) {
			if (isUnder(absPath, absolutePath(r))) return std::string();
		}
		return "path is outside the allowed roots";
	}

	Response denyIfOutside(const std::string& absPath) const {
		std::string why = checkAccess(absPath);
		if (why.empty()) return Response();      // status 200 = 通過
		return Response::error(403, why);
	}

	//-----------------------------------------------------------------------
	Response handle(const Request& req) {
		const std::string& op = req.suffix;

		if (op == "roots"    && req.method == "GET")  return opRoots(req);
		if (op == "list"     && req.method == "GET")  return opList(req);
		if (op == "stat"     && req.method == "GET")  return opStat(req);
		if (op == "read"     && req.method == "GET")  return opRead(req);
		if (op == "text"     && req.method == "GET")  return opText(req);
		if (op == "download" && req.method == "GET")  return opDownload(req);
		if (op == "write"    && req.method == "POST") return opWrite(req);
		if (op == "mkdir"    && req.method == "POST") return opMkdir(req);
		if (op == "remove"   && req.method == "POST") return opRemove(req);

		return Response::error(404, "unknown file operation: " + op);
	}

	//-----------------------------------------------------------------------
	Response opRoots(const Request&) {
		Json arr = Json::array();
		auto add = [&arr](const std::string& path, const std::string& label,
		                  const char* kind) {
			std::error_code ec;
			if (!fs::exists(fs::u8path(path), ec)) return;
			Json e = Json::object();
			e.set("path",  Json(normalize(path)));
			e.set("label", Json(label));
			e.set("kind",  Json(std::string(kind)));
			arr.push(std::move(e));
		};

		// --root が指定されていればそれだけを見せる
		if (!opt().roots.empty()) {
			for (const auto& r : opt().roots) {
				std::string abs = absolutePath(r);
				add(abs, abs, "root");
			}
			Json j = Json::object();
			j.set("roots", std::move(arr));
			return Response::json(j);
		}

#ifdef _WIN32
		DWORD mask = ::GetLogicalDrives();
		for (int i = 0; i < 26; ++i) {
			if (!(mask & (1u << i))) continue;
			std::string d;
			d += (char)('A' + i);
			d += ":/";
			add(d, d, "drive");
		}
		{
			char buf[MAX_PATH];
			DWORD n = ::GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
			if (n > 0 && n < MAX_PATH) add(normalize(buf), "Home", "home");
		}
#else
		add("/", "/", "drive");
		if (const char* home = getenv("HOME")) add(home, "Home", "home");
#endif
		{
			std::error_code ec;
			std::string cwd = normalize(fs::current_path(ec).u8string());
			if (!ec) add(cwd, "Current directory", "cwd");
		}

		Json j = Json::object();
		j.set("roots", std::move(arr));
		return Response::json(j);
	}

	//-----------------------------------------------------------------------
	Json entryJson(const fs::directory_entry& e) const {
		std::error_code ec;
		Json o = Json::object();
		o.set("name", Json(normalize(e.path().filename().u8string())));
		o.set("path", Json(normalize(e.path().u8string())));
		bool isDir = e.is_directory(ec);
		o.set("dir",  Json(isDir));
		o.set("link", Json(e.is_symlink(ec)));
		if (!isDir) {
			uint64_t sz = (uint64_t)e.file_size(ec);
			o.set("size", Json(ec ? 0LL : (long long)sz));
		} else {
			o.set("size", Json(0));
		}
		auto t = e.last_write_time(ec);
		o.set("mtime", Json(ec ? 0LL : (long long)fileTimeToUnixMs(t)));
		return o;
	}

	Json listDir(const std::string& absPath) const {
		Json j = Json::object();
		j.set("path", Json(absPath));

		Json entries = Json::array();
		std::error_code ec;
		fs::directory_iterator it(fs::u8path(absPath), ec);
		if (ec) {
			j.set("error", Json(ec.message()));
			j.set("entries", std::move(entries));
			return j;
		}
		for (; it != fs::directory_iterator(); it.increment(ec)) {
			if (ec) break;
			// アクセスできない項目は飛ばす (権限の無いシステムフォルダ等)
			std::error_code e2;
			if (!it->exists(e2)) continue;
			entries.push(entryJson(*it));
		}

		// 並び替えは UI 側の責務 (ユーザが列で切り替えられるようにするため)
		j.set("entries", std::move(entries));

		// 親ディレクトリ
		fs::path parent = fs::u8path(absPath).parent_path();
		std::string p = normalize(parent.u8string());
		if (!p.empty() && p != absPath) j.set("parent", Json(p));
		return j;
	}

	Response opList(const Request& req) {
		std::string abs = absolutePath(req.param("path", "."));
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;
		if (!std::filesystem::is_directory(fs::u8path(abs)))
			return Response::error(404, "not a directory: " + abs);
		return Response::json(listDir(abs));
	}

	//-----------------------------------------------------------------------
	Response opStat(const Request& req) {
		std::string abs = absolutePath(req.param("path"));
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;

		std::error_code ec;
		fs::path p = fs::u8path(abs);
		if (!fs::exists(p, ec)) return Response::error(404, "not found: " + abs);

		Json j = Json::object();
		j.set("path", Json(abs));
		j.set("name", Json(normalize(p.filename().u8string())));
		bool isDir = fs::is_directory(p, ec);
		j.set("dir", Json(isDir));
		j.set("size", Json(isDir ? 0LL : (long long)fs::file_size(p, ec)));
		auto t = fs::last_write_time(p, ec);
		j.set("mtime", Json(ec ? 0LL : (long long)fileTimeToUnixMs(t)));
		return Response::json(j);
	}

	//-----------------------------------------------------------------------
	/// ファイルの一部または全部を読む
	bool readRange(const std::string& abs, uint64_t offset, uint64_t length,
	               std::string& out, std::string& err) const {
		std::error_code ec;
		fs::path p = fs::u8path(abs);
		if (!fs::is_regular_file(p, ec)) { err = "not a regular file"; return false; }
		uint64_t size = (uint64_t)fs::file_size(p, ec);
		if (ec) { err = ec.message(); return false; }
		if (offset > size) { err = "offset beyond end of file"; return false; }
		uint64_t remain = size - offset;
		// windows.h の min/max マクロに食われないよう括弧で囲む
		uint64_t want   = (length == 0) ? remain : (std::min)(length, remain);
		if (want > kDefaultReadLimit) {
			err = "range too large (" + std::to_string(want) + " bytes, limit " +
			      std::to_string(kDefaultReadLimit) + ") — use offset/length";
			return false;
		}
		std::ifstream f(p, std::ios::binary);
		if (!f) { err = "cannot open"; return false; }
		f.seekg((std::streamoff)offset);
		out.assign((size_t)want, '\0');
		if (want) {
			f.read(&out[0], (std::streamsize)want);
			out.resize((size_t)f.gcount());
		}
		return true;
	}

	Response opRead(const Request& req) {
		std::string abs = absolutePath(req.param("path"));
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;

		std::string data, err;
		if (!readRange(abs, (uint64_t)req.paramInt("offset", 0),
		               (uint64_t)req.paramInt("length", 0), data, err))
			return Response::error(err.find("too large") != std::string::npos ? 413 : 404, err);
		return Response::bytes(std::move(data), "application/octet-stream");
	}

	Response opDownload(const Request& req) {
		std::string abs = absolutePath(req.param("path"));
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;

		std::string data, err;
		if (!readRange(abs, 0, 0, data, err)) return Response::error(404, err);
		std::string fname = normalize(fs::u8path(abs).filename().u8string());
		Response r = Response::bytes(std::move(data), mimeForPath(abs));
		r.attachment(fname);
		return r;
	}

	//-----------------------------------------------------------------------
	/// UTF-8 として妥当か
	static bool isValidUtf8(const std::string& s) {
		size_t i = 0;
		while (i < s.size()) {
			unsigned char c = (unsigned char)s[i];
			int n;
			if (c < 0x80)              { i += 1; continue; }
			else if ((c & 0xE0) == 0xC0) n = 1;
			else if ((c & 0xF0) == 0xE0) n = 2;
			else if ((c & 0xF8) == 0xF0) n = 3;
			else return false;
			if (i + (size_t)n >= s.size()) return false;   // 継続バイトが足りない
			for (int k = 1; k <= n; ++k) {
				if (((unsigned char)s[i + (size_t)k] & 0xC0) != 0x80) return false;
			}
			i += (size_t)n + 1;
		}
		return true;
	}

	/// UTF-8 でなければ既定のマルチバイト (Windows なら CP932 等) とみなして変換する
	static std::string toUtf8(const std::string& raw, std::string& encodingOut) {
		if (isValidUtf8(raw)) { encodingOut = "utf-8"; return raw; }
#ifdef _WIN32
		int wlen = ::MultiByteToWideChar(932, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
		if (wlen > 0) {
			std::wstring w((size_t)wlen, L'\0');
			::MultiByteToWideChar(932, 0, raw.c_str(), (int)raw.size(), &w[0], wlen);
			int ulen = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen,
			                                 nullptr, 0, nullptr, nullptr);
			if (ulen > 0) {
				std::string u((size_t)ulen, '\0');
				::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, &u[0], ulen,
				                      nullptr, nullptr);
				encodingOut = "cp932";
				return u;
			}
		}
#endif
		// 変換できなければ Latin-1 として通す (少なくとも壊れた JSON にはならない)
		std::string out;
		out.reserve(raw.size() * 2);
		for (unsigned char c : raw) {
			if (c < 0x80) out += (char)c;
			else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
		}
		encodingOut = "latin-1";
		return out;
	}

	Response opText(const Request& req) {
		std::string abs = absolutePath(req.param("path"));
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;

		uint64_t limit = (uint64_t)req.paramInt("length", 1024 * 1024);
		std::string data, err;
		if (!readRange(abs, (uint64_t)req.paramInt("offset", 0), limit, data, err))
			return Response::error(404, err);

		// 部分読みの切れ端で文字コード判定を誤らないよう、末尾の不完全な
		// UTF-8 シーケンスを落としてから判定する
		size_t rawBytes = data.size();
		trimIncompleteUtf8Tail(data);

		std::string enc;
		std::string text = toUtf8(data, enc);
		Json j = Json::object();
		j.set("path", Json(abs));
		j.set("encoding", Json(enc));
		j.set("bytes", Json((long long)rawBytes));
		j.set("text", Json(std::move(text)));
		return Response::json(j);
	}

	//-----------------------------------------------------------------------
	static bool base64Decode(const std::string& in, std::string& out) {
		auto val = [](char c) -> int {
			if (c >= 'A' && c <= 'Z') return c - 'A';
			if (c >= 'a' && c <= 'z') return c - 'a' + 26;
			if (c >= '0' && c <= '9') return c - '0' + 52;
			if (c == '+') return 62;
			if (c == '/') return 63;
			return -1;
		};
		out.clear();
		int buf = 0, bits = 0;
		for (char c : in) {
			if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
			int v = val(c);
			if (v < 0) return false;
			buf = (buf << 6) | v;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out += (char)((buf >> bits) & 0xFF);
			}
		}
		return true;
	}

	Response opWrite(const Request& req) {
		if (!opt().allowWrite)
			return Response::error(403, "write access is disabled (start with --allow-write)");

		const Json& j = req.json();
		std::string abs = absolutePath(j["path"].asStr());
		if (abs.empty()) return Response::error(400, "path is required");
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;

		std::string data;
		if (j.has("base64")) {
			if (!base64Decode(j["base64"].asStr(), data))
				return Response::error(400, "invalid base64 payload");
		} else if (j.has("text")) {
			data = j["text"].asStr();
		} else {
			return Response::error(400, "either 'text' or 'base64' is required");
		}

		std::error_code ec;
		fs::create_directories(fs::u8path(abs).parent_path(), ec);
		std::ofstream f(fs::u8path(abs), std::ios::binary | std::ios::trunc);
		if (!f) return Response::error(500, "cannot open for writing: " + abs);
		if (!data.empty()) f.write(data.data(), (std::streamsize)data.size());
		if (!f) return Response::error(500, "write failed: " + abs);

		logI("fs: wrote " + std::to_string(data.size()) + " bytes to " + abs);
		Json out = Json::object();
		out.set("path", Json(abs));
		out.set("bytes", Json((long long)data.size()));
		return Response::json(out);
	}

	Response opMkdir(const Request& req) {
		if (!opt().allowWrite)
			return Response::error(403, "write access is disabled (start with --allow-write)");
		std::string abs = absolutePath(req.json()["path"].asStr());
		if (abs.empty()) return Response::error(400, "path is required");
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;

		std::error_code ec;
		fs::create_directories(fs::u8path(abs), ec);
		if (ec) return Response::error(500, ec.message());
		Json out = Json::object();
		out.set("path", Json(abs));
		return Response::json(out);
	}

	Response opRemove(const Request& req) {
		if (!opt().allowWrite)
			return Response::error(403, "write access is disabled (start with --allow-write)");
		const Json& j = req.json();
		std::string abs = absolutePath(j["path"].asStr());
		if (abs.empty()) return Response::error(400, "path is required");
		Response deny = denyIfOutside(abs);
		if (deny.status != 200) return deny;

		std::error_code ec;
		uintmax_t n = j["recursive"].asBool(false)
			? fs::remove_all(fs::u8path(abs), ec)
			: (fs::remove(fs::u8path(abs), ec) ? 1u : 0u);
		if (ec) return Response::error(500, ec.message());
		logI("fs: removed " + abs + " (" + std::to_string((unsigned long long)n) + " entries)");
		Json out = Json::object();
		out.set("removed", Json((long long)n));
		return Response::json(out);
	}
};

} // anonymous

//---------------------------------------------------------------------------
std::unique_ptr<IModule> makeFsModule()
{
	return std::unique_ptr<IModule>(new FsModule());
}

} // namespace appserve
