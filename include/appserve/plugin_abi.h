//---------------------------------------------------------------------------
// 動的プラグイン (DLL / so) の C ABI
//
// C++ の型を DLL 境界で跨がせないため、やり取りは POD 構造体と関数ポインタ
// のみで行う。プラグインは appserve_plugin_init を 1 つだけ export する。
//
//   #include <appserve/plugin_abi.h>
//   static void my_handler(const AppserveReq* req, AppserveResp* resp, void* user) {
//       static const char kBody[] = "{\"hello\":1}";
//       resp->status   = 200;
//       resp->mime     = appserve_str("application/json");
//       resp->body     = appserve_str(kBody);
//       resp->free_body = 0;      /* 静的なので解放不要 */
//   }
//   APPSERVE_EXPORT int appserve_plugin_init(const AppserveHost* host) {
//       if (host->abi_version != APPSERVE_ABI_VERSION) return 0;
//       host->route(host->ctx, "/api/mine/", APPSERVE_AFFINITY_MAIN, my_handler, 0);
//       return 1;
//   }
//---------------------------------------------------------------------------
#ifndef APPSERVE_PLUGIN_ABI_H
#define APPSERVE_PLUGIN_ABI_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APPSERVE_ABI_VERSION 1

#define APPSERVE_AFFINITY_MAIN 0
#define APPSERVE_AFFINITY_ANY  1

#define APPSERVE_LOG_VERBOSE 0
#define APPSERVE_LOG_DEBUG   1
#define APPSERVE_LOG_INFO    2
#define APPSERVE_LOG_WARN    3
#define APPSERVE_LOG_ERROR   4

#if defined(_WIN32)
#  define APPSERVE_EXPORT __declspec(dllexport)
#else
#  define APPSERVE_EXPORT __attribute__((visibility("default")))
#endif

/* 長さ付き文字列 (NUL 終端を仮定しない。バイナリボディも運べる) */
typedef struct AppserveStr {
	const char* ptr;
	size_t      len;
} AppserveStr;

typedef struct AppserveReq {
	AppserveStr method;
	AppserveStr path;
	AppserveStr prefix;
	AppserveStr suffix;
	AppserveStr query;
	AppserveStr body;
	/* ヘッダ取得 (キーは小文字)。未定義なら ptr=NULL */
	AppserveStr (*header)(const struct AppserveReq* self, const char* key);
	void*       impl;    /* ホスト内部 */
} AppserveReq;

typedef struct AppserveResp {
	int         status;      /* 既定 200 */
	AppserveStr mime;        /* 空なら application/json */
	AppserveStr body;
	/* body を解放する関数。静的/不要なら NULL。ホストがコピー後に呼ぶ。 */
	void      (*free_body)(void* ptr);
} AppserveResp;

typedef void (*AppserveHandlerFn)(const AppserveReq* req, AppserveResp* resp, void* user);
typedef void (*AppserveReplFn)(const char* args, AppserveResp* resp, void* user);

typedef struct AppserveHost {
	int   abi_version;
	void* ctx;

	void (*route)(void* ctx, const char* prefix, int affinity,
	              AppserveHandlerFn fn, void* user);
	void (*repl_command)(void* ctx, const char* name, const char* help,
	                     AppserveReplFn fn, void* user);
	void (*broadcast)(void* ctx, const char* channel, const char* payload);
	void (*log)(void* ctx, int level, const char* message);
	/* アプリ情報 */
	int  (*port)(void* ctx);
	const char* (*app_name)(void* ctx);
} AppserveHost;

/* プラグインが export する唯一のシンボル。1 を返せば成功。 */
APPSERVE_EXPORT int appserve_plugin_init(const AppserveHost* host);

#ifdef __cplusplus
} /* extern "C" */

/* C++ から使うときの糖衣 */
inline AppserveStr appserve_str(const char* s) {
	AppserveStr r;
	r.ptr = s;
	r.len = s ? strlen(s) : 0;
	return r;
}
#endif

#endif /* APPSERVE_PLUGIN_ABI_H */
