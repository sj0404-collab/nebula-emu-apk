#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
int main(void) {
  WSADATA wd; WSAStartup(MAKEWORD(2,2), &wd);
  struct addrinfo hints = {0}, *ai = 0; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo("example.com", "80", &hints, &ai)) { printf("resolve failed\n"); return 1; }
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
  int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen); printf("connect -> %d err=%d\n", r, WSAGetLastError());
  fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(s, &wf); FD_SET(s, &ef); struct timeval tv = {10, 0};
  r = select(1, NULL, &wf, &ef, &tv); printf("select(w) -> %d writable=%d except=%d\n", r, FD_ISSET(s, &wf) != 0, FD_ISSET(s, &ef) != 0);
  const char *req = "HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n"; send(s, req, (int)strlen(req), 0);
  fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf); tv.tv_sec = 10; tv.tv_usec = 0;
  r = select(1, &rf, NULL, NULL, &tv); printf("select(r) -> %d readable=%d\n", r, FD_ISSET(s, &rf) != 0);
  char buf[256]; int n = recv(s, buf, 255, 0); if (n > 0) { buf[n] = 0; char *e = strstr(buf, "\r\n"); if (e) *e = 0; printf("recv %d: %s\n", n, buf); } else printf("recv -> %d err=%d\n", n, WSAGetLastError());
  WSAPOLLFD pf = { s, POLLRDNORM, 0 }; r = WSAPoll(&pf, 1, 100); printf("WSAPoll -> %d revents=0x%x\n", r, pf.revents);
  closesocket(s); return 0;
}
