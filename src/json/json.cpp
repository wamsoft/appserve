//---------------------------------------------------------------------------
// 最小 JSON 実装
//---------------------------------------------------------------------------
#include "appserve/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace appserve {

const Json Json::kNull;

//---------------------------------------------------------------------------
Json Json::array()
{
	Json j;
	j.type_ = Type::Arr;
	return j;
}

Json Json::object()
{
	Json j;
	j.type_ = Type::Obj;
	return j;
}

Json Json::array(const std::vector<std::string>& v)
{
	Json j = array();
	for (const auto& s : v) j.push(Json(s));
	return j;
}

//---------------------------------------------------------------------------
bool Json::asBool(bool def) const
{
	switch (type_) {
		case Type::Bool: return bool_;
		case Type::Int:  return int_ != 0;
		case Type::Real: return real_ != 0.0;
		case Type::Str:  return !str_.empty() && str_ != "0" && str_ != "false";
		default:         return def;
	}
}

int64_t Json::asInt(int64_t def) const
{
	switch (type_) {
		case Type::Bool: return bool_ ? 1 : 0;
		case Type::Int:  return int_;
		case Type::Real: return (int64_t)real_;
		case Type::Str:  return str_.empty() ? def : (int64_t)strtoll(str_.c_str(), nullptr, 10);
		default:         return def;
	}
}

double Json::asReal(double def) const
{
	switch (type_) {
		case Type::Bool: return bool_ ? 1.0 : 0.0;
		case Type::Int:  return (double)int_;
		case Type::Real: return real_;
		case Type::Str:  return str_.empty() ? def : strtod(str_.c_str(), nullptr);
		default:         return def;
	}
}

std::string Json::asStr(const std::string& def) const
{
	switch (type_) {
		case Type::Str:  return str_;
		case Type::Bool: return bool_ ? "true" : "false";
		case Type::Int:  return std::to_string(int_);
		case Type::Real: { char b[40]; snprintf(b, sizeof(b), "%.17g", real_); return b; }
		default:         return def;
	}
}

size_t Json::size() const
{
	if (type_ == Type::Arr) return arr_.size();
	if (type_ == Type::Obj) return obj_.size();
	return 0;
}

//---------------------------------------------------------------------------
void Json::push(Json v)
{
	if (type_ != Type::Arr) { *this = array(); }
	arr_.push_back(std::move(v));
}

const Json& Json::at(size_t i) const
{
	if (type_ != Type::Arr || i >= arr_.size()) return kNull;
	return arr_[i];
}

bool Json::has(const std::string& key) const
{
	if (type_ != Type::Obj) return false;
	for (const auto& kv : obj_) if (kv.first == key) return true;
	return false;
}

const Json& Json::at(const std::string& key) const
{
	if (type_ != Type::Obj) return kNull;
	for (const auto& kv : obj_) if (kv.first == key) return kv.second;
	return kNull;
}

Json& Json::operator[](const std::string& key)
{
	if (type_ != Type::Obj) {
		// null / 他型からの書き込みはオブジェクト化する
		type_ = Type::Obj;
		arr_.clear();
		str_.clear();
	}
	for (auto& kv : obj_) if (kv.first == key) return kv.second;
	obj_.emplace_back(key, Json());
	return obj_.back().second;
}

void Json::set(std::string key, Json v)
{
	(*this)[key] = std::move(v);
}

//---------------------------------------------------------------------------
std::string Json::quote(const std::string& s)
{
	std::string o;
	o.reserve(s.size() + 2);
	o += '"';
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
			case '"':  o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n";  break;
			case '\r': o += "\\r";  break;
			case '\t': o += "\\t";  break;
			case '\b': o += "\\b";  break;
			case '\f': o += "\\f";  break;
			default:
				if (c < 0x20) {
					char b[8];
					snprintf(b, sizeof(b), "\\u%04x", c);
					o += b;
				} else {
					// UTF-8 はそのまま通す (Content-Type で charset=utf-8 を宣言する)
					o += (char)c;
				}
		}
	}
	o += '"';
	return o;
}

//---------------------------------------------------------------------------
namespace {
void indentTo(std::string& o, int indent, int depth)
{
	if (indent < 0) return;
	o += '\n';
	o.append((size_t)(indent * depth), ' ');
}
} // anonymous

void Json::dumpTo(std::string& o, int indent, int depth) const
{
	switch (type_) {
		case Type::Null: o += "null"; return;
		case Type::Bool: o += bool_ ? "true" : "false"; return;
		case Type::Int:  o += std::to_string(int_); return;
		case Type::Real: {
			if (std::isnan(real_) || std::isinf(real_)) { o += "null"; return; }
			char b[40];
			snprintf(b, sizeof(b), "%.17g", real_);
			// 整数値になったときも JSON の数値として妥当な形にする
			o += b;
			return; }
		case Type::Str: o += quote(str_); return;
		case Type::Arr: {
			if (arr_.empty()) { o += "[]"; return; }
			o += '[';
			for (size_t i = 0; i < arr_.size(); ++i) {
				if (i) o += ',';
				indentTo(o, indent, depth + 1);
				arr_[i].dumpTo(o, indent, depth + 1);
			}
			indentTo(o, indent, depth);
			o += ']';
			return; }
		case Type::Obj: {
			if (obj_.empty()) { o += "{}"; return; }
			o += '{';
			for (size_t i = 0; i < obj_.size(); ++i) {
				if (i) o += ',';
				indentTo(o, indent, depth + 1);
				o += quote(obj_[i].first);
				o += ':';
				if (indent >= 0) o += ' ';
				obj_[i].second.dumpTo(o, indent, depth + 1);
			}
			indentTo(o, indent, depth);
			o += '}';
			return; }
	}
}

std::string Json::dump(int indent) const
{
	std::string o;
	dumpTo(o, indent, 0);
	return o;
}

//---------------------------------------------------------------------------
// パーサ
//---------------------------------------------------------------------------
namespace {

struct Parser {
	const char* p;
	const char* end;
	std::string err;

	void skipWs()
	{
		while (p < end) {
			char c = *p;
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
			else break;
		}
	}

	bool fail(const char* msg)
	{
		if (err.empty()) err = msg;
		return false;
	}

	// UTF-16 コードポイントを UTF-8 へ (サロゲートペア対応)
	static void appendUtf8(std::string& o, unsigned cp)
	{
		if (cp < 0x80) {
			o += (char)cp;
		} else if (cp < 0x800) {
			o += (char)(0xC0 | (cp >> 6));
			o += (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			o += (char)(0xE0 | (cp >> 12));
			o += (char)(0x80 | ((cp >> 6) & 0x3F));
			o += (char)(0x80 | (cp & 0x3F));
		} else {
			o += (char)(0xF0 | (cp >> 18));
			o += (char)(0x80 | ((cp >> 12) & 0x3F));
			o += (char)(0x80 | ((cp >> 6) & 0x3F));
			o += (char)(0x80 | (cp & 0x3F));
		}
	}

	bool hex4(unsigned& out)
	{
		if (end - p < 4) return fail("truncated \\u escape");
		out = 0;
		for (int i = 0; i < 4; ++i) {
			char c = p[i];
			unsigned d;
			if (c >= '0' && c <= '9')      d = (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
			else return fail("bad \\u escape");
			out = (out << 4) | d;
		}
		p += 4;
		return true;
	}

	bool parseString(std::string& out)
	{
		if (p >= end || *p != '"') return fail("expected string");
		++p;
		out.clear();
		while (p < end) {
			char c = *p++;
			if (c == '"') return true;
			if (c != '\\') { out += c; continue; }
			if (p >= end) return fail("truncated escape");
			char e = *p++;
			switch (e) {
				case '"':  out += '"';  break;
				case '\\': out += '\\'; break;
				case '/':  out += '/';  break;
				case 'b':  out += '\b'; break;
				case 'f':  out += '\f'; break;
				case 'n':  out += '\n'; break;
				case 'r':  out += '\r'; break;
				case 't':  out += '\t'; break;
				case 'u': {
					unsigned cp;
					if (!hex4(cp)) return false;
					if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 &&
					    p[0] == '\\' && p[1] == 'u') {
						const char* save = p;
						p += 2;
						unsigned lo;
						if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
							cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						} else {
							p = save;   // 単独サロゲートはそのまま出す
						}
					}
					appendUtf8(out, cp);
					break; }
				default: return fail("bad escape");
			}
		}
		return fail("unterminated string");
	}

	bool parseValue(Json& out, int depth)
	{
		if (depth > 200) return fail("too deep");
		skipWs();
		if (p >= end) return fail("unexpected end");
		char c = *p;
		if (c == '{') {
			++p;
			out = Json::object();
			skipWs();
			if (p < end && *p == '}') { ++p; return true; }
			while (true) {
				skipWs();
				std::string key;
				if (!parseString(key)) return false;
				skipWs();
				if (p >= end || *p != ':') return fail("expected ':'");
				++p;
				Json v;
				if (!parseValue(v, depth + 1)) return false;
				out.set(std::move(key), std::move(v));
				skipWs();
				if (p < end && *p == ',') { ++p; continue; }
				if (p < end && *p == '}') { ++p; return true; }
				return fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			++p;
			out = Json::array();
			skipWs();
			if (p < end && *p == ']') { ++p; return true; }
			while (true) {
				Json v;
				if (!parseValue(v, depth + 1)) return false;
				out.push(std::move(v));
				skipWs();
				if (p < end && *p == ',') { ++p; continue; }
				if (p < end && *p == ']') { ++p; return true; }
				return fail("expected ',' or ']'");
			}
		}
		if (c == '"') {
			std::string s;
			if (!parseString(s)) return false;
			out = Json(std::move(s));
			return true;
		}
		if ((size_t)(end - p) >= 4 && memcmp(p, "true", 4) == 0)  { p += 4; out = Json(true);  return true; }
		if ((size_t)(end - p) >= 5 && memcmp(p, "false", 5) == 0) { p += 5; out = Json(false); return true; }
		if ((size_t)(end - p) >= 4 && memcmp(p, "null", 4) == 0)  { p += 4; out = Json();      return true; }
		// 数値
		{
			const char* start = p;
			if (p < end && (*p == '-' || *p == '+')) ++p;
			bool isReal = false;
			while (p < end) {
				char d = *p;
				if (d >= '0' && d <= '9') { ++p; continue; }
				if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
					isReal = true;
					++p;
					continue;
				}
				break;
			}
			if (p == start) return fail("unexpected character");
			std::string num(start, (size_t)(p - start));
			if (isReal) out = Json(strtod(num.c_str(), nullptr));
			else        out = Json((long long)strtoll(num.c_str(), nullptr, 10));
			return true;
		}
	}
};

} // anonymous

bool Json::parse(const std::string& text, Json& out, std::string* err)
{
	Parser ps;
	ps.p   = text.data();
	ps.end = text.data() + text.size();
	Json v;
	if (!ps.parseValue(v, 0)) {
		if (err) *err = ps.err.empty() ? "parse error" : ps.err;
		return false;
	}
	ps.skipWs();
	if (ps.p != ps.end) {
		if (err) *err = "trailing data";
		return false;
	}
	out = std::move(v);
	return true;
}

Json Json::parse(const std::string& text)
{
	Json v;
	if (!parse(text, v, nullptr)) return Json();
	return v;
}

} // namespace appserve
