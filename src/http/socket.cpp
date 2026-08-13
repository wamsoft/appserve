//---------------------------------------------------------------------------
// ソケット差分吸収 実装
//---------------------------------------------------------------------------
#include "http/socket.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace appserve {
namespace net {

namespace {
std::mutex g_init_mu;
int        g_init_count = 0;
} // anonymous

//---------------------------------------------------------------------------
bool globalInit()
{
	std::lock_guard<std::mutex> lk(g_init_mu);
	if (g_init_count++ > 0) return true;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		g_init_count = 0;
		return false;
	}
#endif
	return true;
}

void globalCleanup()
{
	std::lock_guard<std::mutex> lk(g_init_mu);
	if (g_init_count <= 0) return;
	if (--g_init_count > 0) return;
#ifdef _WIN32
	WSACleanup();
#endif
}

//---------------------------------------------------------------------------
void closeSocket(sock_t s)
{
	if (s == APPSERVE_SOCK_INVALID) return;
#ifdef _WIN32
	::closesocket(s);
#else
	::close(s);
#endif
}

bool setRecvTimeout(sock_t s, int ms)
{
#ifdef _WIN32
	DWORD tv = (DWORD)ms;
	return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == 0;
#else
	struct timeval tv;
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == 0;
#endif
}

bool setNoDelay(sock_t s)
{
	int one = 1;
	return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one)) == 0;
}

//---------------------------------------------------------------------------
bool sendAll(sock_t s, const char* buf, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
#ifdef _WIN32
		int n = ::send(s, buf + sent, (int)(len - sent), 0);
#else
		// SIGPIPE でプロセスが死なないように MSG_NOSIGNAL (Linux)
#  ifdef MSG_NOSIGNAL
		ssize_t n = ::send(s, buf + sent, len - sent, MSG_NOSIGNAL);
#  else
		ssize_t n = ::send(s, buf + sent, len - sent, 0);
#  endif
#endif
		if (n <= 0) return false;
		sent += (size_t)n;
	}
	return true;
}

bool lastErrorWasTimeout()
{
#ifdef _WIN32
	int e = WSAGetLastError();
	return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
}

//---------------------------------------------------------------------------
// UDP ソケットをダミー宛先へ connect() すると (パケットは飛ばない)、経路選択で
// 使われるローカルアドレスが getsockname() で得られる。getifaddrs が無い環境
// でも動く手法。
std::string outwardIPv4()
{
	std::string result;
	sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (s == APPSERVE_SOCK_INVALID) return result;

	sockaddr_in peer;
	std::memset(&peer, 0, sizeof(peer));
	peer.sin_family      = AF_INET;
	peer.sin_port        = htons(53);
	peer.sin_addr.s_addr = htonl(0x08080808u);   // 8.8.8.8 (到達性不要)

	if (::connect(s, (sockaddr*)&peer, sizeof(peer)) == 0) {
		sockaddr_in local;
		std::memset(&local, 0, sizeof(local));
#ifdef _WIN32
		int len = sizeof(local);
#else
		socklen_t len = sizeof(local);
#endif
		if (::getsockname(s, (sockaddr*)&local, &len) == 0) {
			unsigned long a = ntohl(local.sin_addr.s_addr);
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
			              (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
			result = buf;
		}
	}
	closeSocket(s);

	if (result == "0.0.0.0" || result.compare(0, 4, "127.") == 0) result.clear();
	return result;
}

} // namespace net
} // namespace appserve
