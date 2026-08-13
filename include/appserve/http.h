//---------------------------------------------------------------------------
// HTTP リクエスト / レスポンス / ハンドラ
//---------------------------------------------------------------------------
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "appserve/json.h"

namespace appserve {

//---------------------------------------------------------------------------
/// ハンドラをどのスレッドで実行するか。
///   Main = メインスレッドへ運んで直列実行 (既定)。スレッドセーフでない C++
///          ライブラリ (psdparse 等) を安全に API 化するための既定値。
///   Any  = 接続スレッドでそのまま実行。速いが自前で排他が必要。
enum class Affinity { Main, Any };

//---------------------------------------------------------------------------
/// ヘッダ表。キーは小文字に正規化される。
class Headers {
public:
	void set(std::string key, std::string value);
	void add(std::string key, std::string value);   // 同名を上書きしない
	bool has(const std::string& key) const;
	/// 未定義なら def
	std::string get(const std::string& key, const std::string& def = std::string()) const;
	const std::vector<std::pair<std::string, std::string>>& all() const { return items_; }
	void clear() { items_.clear(); }
private:
	std::vector<std::pair<std::string, std::string>> items_;
};

//---------------------------------------------------------------------------
class Request {
public:
	std::string method;    ///< "GET" / "POST" ...
	std::string path;      ///< URL デコード済み・クエリ除去済みのパス
	std::string prefix;    ///< マッチしたルートの prefix
	std::string suffix;    ///< path から prefix を取り除いた残り
	std::string query;     ///< 生クエリ文字列 ("a=1&b=2")
	std::string body;      ///< 生ボディ (バイナリ可)
	Headers     headers;
	std::string remote;    ///< 接続元 ("127.0.0.1")

	/// クエリパラメータ (URL デコード済み)。未定義なら def。
	std::string param(const std::string& key, const std::string& def = std::string()) const;
	bool        hasParam(const std::string& key) const;
	/// クエリを整数として取得
	int64_t     paramInt(const std::string& key, int64_t def = 0) const;

	/// body を JSON としてパースした結果 (初回アクセスで遅延パース)。
	/// パースできなければ null。
	const Json& json() const;

private:
	mutable bool jsonParsed_ = false;
	mutable Json json_;
};

//---------------------------------------------------------------------------
class Response {
public:
	int         status = 200;
	std::string mime   = "application/json; charset=utf-8";
	std::string body;
	Headers     headers;

	static Response json(const Json& v);
	static Response text(std::string s);
	static Response html(std::string s);
	static Response bytes(std::string data, std::string mime);
	/// {"error":"message"} 形式のエラー応答
	static Response error(int status, const std::string& message);
	static Response noContent();
	/// ファイルダウンロード用に Content-Disposition を付ける
	Response& attachment(const std::string& filename);
};

using Handler = std::function<Response(const Request&)>;

/// ステータスコード → 標準の理由句
const char* statusText(int status);
/// 拡張子から MIME を推定
std::string mimeForPath(const std::string& path);
/// %XX のデコード ('+' を空白にするかは plusIsSpace で選択)
std::string urlDecode(const std::string& s, bool plusIsSpace = false);
std::string urlEncode(const std::string& s);
/// "a=1&b=2" をパースする
std::map<std::string, std::string> parseQuery(const std::string& query);

} // namespace appserve
