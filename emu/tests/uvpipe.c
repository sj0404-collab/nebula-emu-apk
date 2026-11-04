/* uvpipe.exe — replays what libuv (node/bun) does to read a child process stdout on Windows. */
#include <windows.h>
#include <stdio.h>
int main(void) {
  char name[128]; sprintf(name, "\\\\?\\pipe\\uv\\%p-%lu", (void*)0x1234, GetCurrentProcessId());
  HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE | WRITE_DAC, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, NULL);
  printf("server=%p err=%lu\n", server, GetLastError());
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE | WRITE_DAC, 0, &sa, OPEN_EXISTING, 0, NULL);
  printf("client=%p err=%lu\n", client, GetLastError());
  OVERLAPPED cov = {0}; ConnectNamedPipe(server, &cov); printf("connect err=%lu\n", GetLastError());
  HANDLE iocp = CreateIoCompletionPort(server, NULL, 42, 0); printf("iocp=%p\n", iocp);
  STARTUPINFOA si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = client; si.hStdError = client;
  PROCESS_INFORMATION pi; char cmd[] = "C:\\Windows\\System32\\cmd.exe /c \"echo hi from child\"";
  BOOL ok = CreateProcessA("C:\\Windows\\System32\\cmd.exe", cmd, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi);
  printf("CreateProcess -> %d\n", ok); CloseHandle(client);
  OVERLAPPED ov = {0}; char buf[256]; DWORD n = 0;
  BOOL r = ReadFile(server, buf, 0, &n, &ov); printf("zero ReadFile -> %d err=%lu\n", r, GetLastError());
  DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED *pov = NULL;
  BOOL g = GetQueuedCompletionStatus(iocp, &bytes, &key, &pov, 5000); printf("GQCS -> %d bytes=%lu key=%lu pov=%p err=%lu\n", g, bytes, (unsigned long)key, pov, GetLastError());
  OVERLAPPED ov2 = {0}; r = ReadFile(server, buf, 255, &n, &ov2); printf("ReadFile -> %d n=%lu err=%lu\n", r, n, GetLastError());
  if (!r && GetLastError() == ERROR_IO_PENDING) { g = GetQueuedCompletionStatus(iocp, &bytes, &key, &pov, 5000); printf("GQCS2 -> %d bytes=%lu\n", g, bytes); n = bytes; }
  buf[n] = 0; printf("data: [%s]\n", buf);
  WaitForSingleObject(pi.hProcess, 5000); DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); printf("exit %lu\n", code);
  return 0;
}
