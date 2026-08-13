//---------------------------------------------------------------------------
// HTTP のリクエスト/レスポンス補助 (URL デコード / クエリ / ヘッダ / 応答生成)
//---------------------------------------------------------------------------
#include "appserve/http.h"
#include "core/util.h"

#include <cctype>
#include <cstdlib>

namespace appserve {

//---------------------------------------------------------------------------
// Headers
//---------------------------------------------------------------------------
void Headers::set(std::string key, std::string value)
{
	key = util::toLower(std::move(key));
	for (auto& kv : items_) {
		if (kv.first == key) { kv.second = std::move(value); return; }
	}
	items_.emplace_back(std::move(key), std::move(value));
}

void Headers::add(std::string key, std::string value)
{
	items_.emplace_back(util::toLower(std::move(key)), std::move(value));
}

bool Headers::has(const std::string& key) const
{
	std::string k = util::toLower(key);
	for (const auto& kv : items_) if (kv.first == k) return true;
	return false;
}

std::string Headers::get(const std::string& key, const std::string& def) const
{
	std::string k = util::toLower(key);
	for (const auto& kv : items_) if (kv.first == k) return kv.second;
	return def;
}

//---------------------------------------------------------------------------
// URL
//---------------------------------------------------------------------------
std::string urlDecode(const std::string& s, bool plusIsSpace)
{
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		return (tolower((unsigned char)c) - 'a') + 10;
	};
	std::string o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c == '%' && i + 2 < s.size() &&
		    isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
			o += (char)((hex(s[i + 1]) << 4) | hex(s[i + 2]));
			i += 2;
		} else if (plusIsSpace && c == '+') {
			o += ' ';
		} else {
			o += c;
		}
	}
	return o;
}

std::string urlEncode(const std::string& s)
{
	static const char* kHex = "0123456789ABCDEF";
	std::string o;
	o.reserve(s.size() * 3 / 2);
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
			o += (char)c;
		} else {
			o += '%';
			o += kHex[c >> 4];
			o += kHex[c & 0x0F];
		}
	}
	return o;
}

std::map<std::string, std::string> parseQuery(const std::string& query)
{
	std::map<std::string, std::string> out;
	size_t start = 0;
	while (start < query.size()) {
		size_t amp = query.find('&', start);
		std::string pair = (amp == std::string::npos)
			? query.substr(start) : query.substr(start, amp - start);
		if (!pair.empty()) {
			size_t eq = pair.find('=');
			if (eq == std::string::npos) {
				out[urlDecode(pair, true)] = std::string();
			} else {
				out[urlDecode(pair.substr(0, eq), true)] =
					urlDecode(pair.substr(eq + 1), true);
			}
		}
		if (amp == std::string::npos) break;
		start = amp + 1;
	}
	return out;
}

//---------------------------------------------------------------------------
// Request
//---------------------------------------------------------------------------
std::string Request::param(const std::string& key, const std::string& def) const
{
	// クエリは多くても数件なのでその場で走査する (辞書を作らない)
	size_t start = 0;
	while (start < query.size()) {
		size_t amp = query.find('&', start);
		std::string pair = (amp == std::string::npos)
			? query.substr(start) : query.substr(start, amp - start);
		size_t eq = pair.find('=');
		std::string k = urlDecode(eq == std::string::npos ? pair : pair.substr(0, eq), true);
		if (k == key) {
			return eq == std::string::npos ? std::string()
			                               : urlDecode(pair.substr(eq + 1), true);
		}
		if (amp == std::string::npos) break;
		start = amp + 1;
	}
	return def;
}

bool Request::hasParam(const std::string& key) const
{
	static const std::string kSentinel = "\x01\x02missing";
	return param(key, kSentinel) != kSentinel;
}

int64_t Request::paramInt(const std::string& key, int64_t def) const
{
	std::string v = param(key);
	if (v.empty()) return def;
	return (int64_t)strtoll(v.c_str(), nullptr, 10);
}

const Json& Request::json() const
{
	if (!jsonParsed_) {
		jsonParsed_ = true;
		if (!body.empty()) {
			Json v;
			if (Json::parse(body, v, nullptr)) json_ = std::move(v);
		}
	}
	return json_;
}

//---------------------------------------------------------------------------
// Response
//---------------------------------------------------------------------------
Response Response::json(const Json& v)
{
	Response r;
	r.status = 200;
	r.mime   = "application/json; charset=utf-8";
	r.body   = v.dump();
	return r;
}

Response Response::text(std::string s)
{
	Response r;
	r.status = 200;
	r.mime   = "text/plain; charset=utf-8";
	r.body   = std::move(s);
	return r;
}

Response Response::html(std::string s)
{
	Response r;
	r.status = 200;
	r.mime   = "text/html; charset=utf-8";
	r.body   = std::move(s);
	return r;
}

Response Response::bytes(std::string data, std::string mime)
{
	Response r;
	r.status = 200;
	r.mime   = std::move(mime);
	r.body   = std::move(data);
	return r;
}

Response Response::error(int status, const std::string& message)
{
	Json j = Json::object();
	j.set("error", Json(message));
	j.set("status", Json(status));
	Response r = json(j);
	r.status = status;
	return r;
}

Response Response::noContent()
{
	Response r;
	r.status = 204;
	r.mime.clear();
	r.body.clear();
	return r;
}

Response& Response::attachment(const std::string& filename)
{
	// ファイル名はヘッダに直接入れられない文字があるので URL エンコードして
	// RFC 5987 の filename* で渡す (ASCII フォールバックも付ける)。
	std::string ascii;
	for (unsigned char c : filename) {
		ascii += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? (char)c : '_';
	}
	headers.set("content-disposition",
		"attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + urlEncode(filename));
	return *this;
}

//---------------------------------------------------------------------------
const char* statusText(int status)
{
	switch (status) {
		case 200: return "OK";
		case 201: return "Created";
		case 204: return "No Content";
		case 206: return "Partial Content";
		case 302: return "Found";
		case 304: return "Not Modified";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 408: return "Request Timeout";
		case 409: return "Conflict";
		case 413: return "Payload Too Large";
		case 415: return "Unsupported Media Type";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 503: return "Service Unavailable";
		default:  return "Status";
	}
}

} // namespace appserve
