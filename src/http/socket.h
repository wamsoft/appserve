//---------------------------------------------------------------------------
// ソケットのプラットフォーム差分吸収
//
// winsock2.h は windows.h より前に include する必要があるため、このヘッダを
// 他の windows ヘッダより先に include すること。
//---------------------------------------------------------------------------
#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define APPSERVE_SOCK_INVALID INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <errno.h>
   typedef int sock_t;
#  define APPSERVE_SOCK_INVALID (-1)
#endif

#include <cstddef>
#include <string>

namespace appserve {
namespace net {

/// プロセスで 1 回だけ必要な初期化 (Windows の WSAStartup)。多重呼び出し可。
bool globalInit();
void globalCleanup();

void closeSocket(sock_t s);
/// 受信タイムアウトを設定する (ミリ秒。0 で無期限)
bool setRecvTimeout(sock_t s, int ms);
bool setNoDelay(sock_t s);

/// len バイトを全部送る。途中で切れたら false。
bool sendAll(sock_t s, const char* buf, size_t len);
inline bool sendStr(sock_t s, const std::string& str) {
	return sendAll(s, str.data(), str.size());
}

/// 直近のエラーが「タイムアウト」か
bool lastErrorWasTimeout();

/// 主要な外向きインタフェースの IPv4 を返す (取得できなければ空)。
/// 0.0.0.0 バインド時に「接続できる URL」を表示するために使う。
std::string outwardIPv4();

} // namespace net
} // namespace appserve
