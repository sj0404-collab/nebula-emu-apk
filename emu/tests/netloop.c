/* Winsock loopback test: large blocking send()/recv() between two threads, one 4 MB overlapped WSASend, shutdown/EOF,
 * select() readiness and FIONREAD. Exercises the emulator's send queue and read-ahead buffer. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static int checks, failed;
#define CHECK(c, m) do { checks++; if (!(c)) { failed++; printf("FAIL %s\n", m); } } while (0)
#define TOTAL (8u << 20)
static unsigned short g_port;
static DWORD WINAPI client(LPVOID p) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(g_port); a.sin_addr.s_addr = htonl(0x7f000001);
  CHECK(connect(s, (struct sockaddr *)&a, sizeof a) == 0, "connect");
  unsigned char *buf = malloc(TOTAL); for (unsigned i = 0; i < TOTAL; i++) buf[i] = (unsigned char)(i * 7 + (i >> 11));
  unsigned sent = 0;                                       /* blocking send() must never return a short count */
  while (sent < TOTAL) { unsigned chunk = TOTAL - sent > 1048576 ? 1048576 : TOTAL - sent; int r = send(s, (char *)buf + sent, (int)chunk, 0); CHECK(r == (int)chunk, "send full chunk"); if (r <= 0) break; sent += (unsigned)r; }
  /* one 4 MB overlapped WSASend: must complete with all bytes */
  WSABUF wb; wb.buf = (char *)buf; wb.len = 4u << 20; DWORD n = 0; WSAOVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = WSACreateEvent();
  int r = WSASend(s, &wb, 1, &n, 0, &ov, NULL); DWORD fl = 0;
  if (r != 0) { CHECK(WSAGetLastError() == WSA_IO_PENDING, "WSASend pending"); CHECK(WSAGetOverlappedResult(s, &ov, &n, TRUE, &fl), "WSAGetOverlappedResult"); }
  CHECK(n == (4u << 20), "overlapped send completed with all bytes");
  shutdown(s, SD_SEND);
  char ack[4]; int g = recv(s, ack, 4, 0); CHECK(g == 2 && ack[0] == 'o' && ack[1] == 'k', "ack from server");
  closesocket(s); free(buf); return 0;
}
int main(void) {
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  SOCKET l = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7f000001);
  CHECK(bind(l, (struct sockaddr *)&a, sizeof a) == 0, "bind"); int al = sizeof a; getsockname(l, (struct sockaddr *)&a, &al); g_port = ntohs(a.sin_port); CHECK(g_port != 0, "ephemeral port");
  CHECK(listen(l, 4) == 0, "listen");
  HANDLE th = CreateThread(NULL, 0, client, NULL, 0, NULL);
  fd_set rs; FD_ZERO(&rs); FD_SET(l, &rs); struct timeval tv = { 20, 0 }; CHECK(select(0, &rs, NULL, NULL, &tv) == 1, "select accept ready");
  SOCKET c = accept(l, NULL, NULL); CHECK(c != INVALID_SOCKET, "accept");
  unsigned long long sum = 0; unsigned got = 0; unsigned char *rb = malloc(65536); int eof = 0;
  while (!eof) { int r = recv(c, (char *)rb, 65536, 0); if (r == 0) { eof = 1; break; } if (r < 0) { printf("recv error %d\n", WSAGetLastError()); break; } for (int i = 0; i < r; i++) sum += rb[i] * (unsigned long long)((got + (unsigned)i) % 251 + 1); got += (unsigned)r; }
  CHECK(got == TOTAL + (4u << 20), "received every byte"); CHECK(eof, "EOF after shutdown");
  unsigned long long want = 0; for (unsigned i = 0; i < TOTAL; i++) want += (unsigned char)(i * 7 + (i >> 11)) * (unsigned long long)(i % 251 + 1);
  for (unsigned i = 0; i < (4u << 20); i++) want += (unsigned char)(i * 7 + (i >> 11)) * (unsigned long long)((TOTAL + i) % 251 + 1);
  CHECK(sum == want, "payload intact and in order");
  unsigned long avail = 1; ioctlsocket(c, FIONREAD, &avail); CHECK(avail == 0, "FIONREAD 0 at EOF");
  CHECK(send(c, "ok", 2, 0) == 2, "ack sent");
  WaitForSingleObject(th, 30000); closesocket(c); closesocket(l); WSACleanup();
  printf("netloop: %d checks, %d failed\n", checks, failed); return failed ? 1 : 0;
}
