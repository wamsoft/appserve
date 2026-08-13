//---------------------------------------------------------------------------
// 内部ユーティリティ
//---------------------------------------------------------------------------
#include "core/util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>

#ifdef _WIN32
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <unistd.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace appserve {
namespace util {

//---------------------------------------------------------------------------
std::string toLower(std::string s)
{
	for (char& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

std::string trim(const std::string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && isspace((unsigned char)s[b])) ++b;
	while (e > b && isspace((unsigned char)s[e - 1])) --e;
	return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const std::string& prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix)
{
	return s.size() >= suffix.size() &&
	       s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& s, char sep)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (true) {
		size_t p = s.find(sep, start);
		if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
		out.push_back(s.substr(start, p - start));
		start = p + 1;
	}
	return out;
}

std::string nextToken(const std::string& s, std::string& rest)
{
	size_t b = 0;
	while (b < s.size() && isspace((unsigned char)s[b])) ++b;
	size_t e = b;
	while (e < s.size() && !isspace((unsigned char)s[e])) ++e;
	std::string tok = s.substr(b, e - b);
	size_t r = e;
	while (r < s.size() && isspace((unsigned char)s[r])) ++r;
	rest = s.substr(r);
	return tok;
}

//---------------------------------------------------------------------------
std::string executablePath()
{
#ifdef _WIN32
	std::wstring buf(1024, L'\0');
	DWORD n = ::GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
	if (n == 0) return std::string();
	buf.resize(n);
	// UTF-16 → UTF-8
	int len = ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), (int)buf.size(),
	                                nullptr, 0, nullptr, nullptr);
	if (len <= 0) return std::string();
	std::string out((size_t)len, '\0');
	::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), (int)buf.size(),
	                      &out[0], len, nullptr, nullptr);
	return normalizeSlash(out);
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buf(size, '\0');
	if (_NSGetExecutablePath(&buf[0], &size) != 0) return std::string();
	buf.resize(strlen(buf.c_str()));
	return buf;
#else
	char buf[4096];
	ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0) return std::string();
	buf[n] = '\0';
	return std::string(buf);
#endif
}

std::string executableDir()
{
	std::string p = executablePath();
	size_t sl = p.find_last_of('/');
	if (sl == std::string::npos) return std::string();
	return p.substr(0, sl);
}

std::string currentDir()
{
	std::error_code ec;
	fs::path p = fs::current_path(ec);
	if (ec) return std::string();
	return normalizeSlash(p.u8string());
}

std::string normalizeSlash(std::string p)
{
	for (char& c : p) if (c == '\\') c = '/';
	return p;
}

std::string joinPath(const std::string& dir, const std::string& rel)
{
	if (dir.empty()) return rel;
	if (rel.empty()) return dir;
	std::string d = dir;
	if (d.back() == '/' || d.back() == '\\') d.pop_back();
	std::string r = rel;
	if (!r.empty() && (r.front() == '/' || r.front() == '\\')) r.erase(0, 1);
	return d + "/" + r;
}

bool isDirectory(const std::string& path)
{
	if (path.empty()) return false;
	std::error_code ec;
	return fs::is_directory(fs::u8path(path), ec);
}

bool isFile(const std::string& path)
{
	if (path.empty()) return false;
	std::error_code ec;
	return fs::is_regular_file(fs::u8path(path), ec);
}

bool readFile(const std::string& path, std::string& out, uint64_t maxSize)
{
	std::error_code ec;
	auto p = fs::u8path(path);
	if (!fs::is_regular_file(p, ec)) return false;
	uint64_t size = (uint64_t)fs::file_size(p, ec);
	if (ec) return false;
	if (maxSize && size > maxSize) return false;
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign((size_t)size, '\0');
	if (size) {
		f.read(&out[0], (std::streamsize)size);
		out.resize((size_t)f.gcount());   // 実際に読めた長さへ切り詰める
	}
	return true;
}

bool writeFile(const std::string& path, const std::string& data)
{
	std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
	if (!f) return false;
	if (!data.empty()) f.write(data.data(), (std::streamsize)data.size());
	return (bool)f;
}

//---------------------------------------------------------------------------
int64_t nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string timestamp()
{
	using namespace std::chrono;
	auto now = system_clock::now();
	std::time_t t = system_clock::to_time_t(now);
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char buf[32];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
	         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
	         tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
	return buf;
}

//---------------------------------------------------------------------------
std::string randomHex(size_t nbytes)
{
	static const char* kHex = "0123456789abcdef";
	std::random_device rd;
	std::mt19937_64 gen((uint64_t)rd() ^ ((uint64_t)rd() << 32) ^ (uint64_t)nowMs());
	std::string out;
	out.reserve(nbytes * 2);
	for (size_t i = 0; i < nbytes; ++i) {
		unsigned b = (unsigned)(gen() & 0xFF);
		out += kHex[b >> 4];
		out += kHex[b & 0x0F];
	}
	return out;
}

} // namespace util
} // namespace appserve
