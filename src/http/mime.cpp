//---------------------------------------------------------------------------
// 拡張子 → MIME
//---------------------------------------------------------------------------
#include "appserve/http.h"
#include "core/util.h"

namespace appserve {

std::string mimeForPath(const std::string& path)
{
	size_t dot   = path.rfind('.');
	size_t slash = path.find_last_of("/\\");
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return "application/octet-stream";
	std::string ext = util::toLower(path.substr(dot + 1));

	// テキスト系
	if (ext == "html" || ext == "htm")  return "text/html; charset=utf-8";
	if (ext == "js"   || ext == "mjs")  return "text/javascript; charset=utf-8";
	if (ext == "css")                   return "text/css; charset=utf-8";
	if (ext == "json" || ext == "map")  return "application/json; charset=utf-8";
	if (ext == "txt"  || ext == "md")   return "text/plain; charset=utf-8";
	if (ext == "csv")                   return "text/csv; charset=utf-8";
	if (ext == "xml")                   return "application/xml; charset=utf-8";
	// 画像
	if (ext == "png")                   return "image/png";
	if (ext == "jpg" || ext == "jpeg")  return "image/jpeg";
	if (ext == "gif")                   return "image/gif";
	if (ext == "webp")                  return "image/webp";
	if (ext == "bmp")                   return "image/bmp";
	if (ext == "svg")                   return "image/svg+xml";
	if (ext == "ico")                   return "image/x-icon";
	// フォント
	if (ext == "woff")                  return "font/woff";
	if (ext == "woff2")                 return "font/woff2";
	if (ext == "ttf")                   return "font/ttf";
	if (ext == "otf")                   return "font/otf";
	// その他
	if (ext == "wasm")                  return "application/wasm";
	if (ext == "pdf")                   return "application/pdf";
	if (ext == "zip")                   return "application/zip";
	if (ext == "mp4")                   return "video/mp4";
	if (ext == "webm")                  return "video/webm";
	if (ext == "mp3")                   return "audio/mpeg";
	if (ext == "wav")                   return "audio/wav";
	if (ext == "ogg")                   return "audio/ogg";

	return "application/octet-stream";
}

} // namespace appserve
