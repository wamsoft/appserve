//---------------------------------------------------------------------------
// 最小 JSON 実装
//
// 外部依存ゼロを維持するための自前実装。API レスポンスの「生成」が主用途で、
// 「読取」はリクエストボディのパースに使う。オブジェクトのキー順は挿入順を
// 保持する (出力が安定してデバッグしやすい)。
//---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace appserve {

class Json {
public:
	enum class Type { Null, Bool, Int, Real, Str, Arr, Obj };

	Json() : type_(Type::Null) {}
	Json(std::nullptr_t) : type_(Type::Null) {}
	Json(bool v) : type_(Type::Bool), bool_(v) {}
	Json(int v) : type_(Type::Int), int_(v) {}
	Json(long v) : type_(Type::Int), int_(v) {}
	Json(long long v) : type_(Type::Int), int_(v) {}
	Json(unsigned v) : type_(Type::Int), int_((int64_t)v) {}
	Json(unsigned long v) : type_(Type::Int), int_((int64_t)v) {}
	Json(unsigned long long v) : type_(Type::Int), int_((int64_t)v) {}
	Json(double v) : type_(Type::Real), real_(v) {}
	Json(const char* v) : type_(Type::Str), str_(v ? v : "") {}
	Json(std::string v) : type_(Type::Str), str_(std::move(v)) {}

	/// 空の配列 / オブジェクトを作る
	static Json array();
	static Json object();
	/// 文字列配列からの生成ヘルパ
	static Json array(const std::vector<std::string>& v);

	Type type() const { return type_; }
	bool isNull() const { return type_ == Type::Null; }
	bool isBool() const { return type_ == Type::Bool; }
	bool isNum()  const { return type_ == Type::Int || type_ == Type::Real; }
	bool isStr()  const { return type_ == Type::Str; }
	bool isArr()  const { return type_ == Type::Arr; }
	bool isObj()  const { return type_ == Type::Obj; }

	bool        asBool(bool def = false) const;
	int64_t     asInt(int64_t def = 0) const;
	double      asReal(double def = 0.0) const;
	std::string asStr(const std::string& def = std::string()) const;

	/// 要素数 (配列/オブジェクト以外は 0)
	size_t size() const;

	// --- 配列 ---
	void push(Json v);
	const Json& at(size_t i) const;
	const Json& operator[](size_t i) const { return at(i); }

	// --- オブジェクト ---
	bool has(const std::string& key) const;
	/// 未定義キーは null を返す (例外を投げない)
	const Json& at(const std::string& key) const;
	const Json& operator[](const std::string& key) const { return at(key); }
	/// 書き込み用。null なら自動でオブジェクト化する
	Json& operator[](const std::string& key);
	void set(std::string key, Json v);
	const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }
	const std::vector<Json>& elements() const { return arr_; }

	/// indent < 0 でコンパクト、>= 0 でその幅のインデント付き整形
	std::string dump(int indent = -1) const;

	/// パース。失敗したら false (err にメッセージ)
	static bool parse(const std::string& text, Json& out, std::string* err = nullptr);
	/// パース失敗時は null を返す簡易版
	static Json parse(const std::string& text);

	/// 文字列を JSON 文字列リテラルとしてエスケープ (引用符込み)
	static std::string quote(const std::string& s);

private:
	void dumpTo(std::string& o, int indent, int depth) const;

	Type        type_;
	bool        bool_ = false;
	int64_t     int_  = 0;
	double      real_ = 0.0;
	std::string str_;
	std::vector<Json> arr_;
	std::vector<std::pair<std::string, Json>> obj_;

	static const Json kNull;
};

} // namespace appserve
