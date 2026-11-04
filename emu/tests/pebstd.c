/* Things runtimes read straight from the PEB / memory manager instead of calling kernel32:
   - Zig's std (OpenTUI's renderer DLL) takes hStdInput/Output/Error from RTL_USER_PROCESS_PARAMETERS
   - JSC / mimalloc decommit heap blocks with VirtualFree(MEM_DECOMMIT) and expect zero pages after re-commit */
#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  unsigned char *peb = (unsigned char *)__readgsqword(0x60);
  unsigned char *params = *(unsigned char **)(peb + 0x20);
  HANDLE in = *(HANDLE *)(params + 0x20), out = *(HANDLE *)(params + 0x28), err = *(HANDLE *)(params + 0x30);
  char line[160]; int n = snprintf(line, sizeof line, "peb std handles: in=%p out=%p err=%p flags=%lu\n", in, out, err, *(unsigned long *)(params + 8));
  DWORD w = 0; BOOL ok = WriteFile(out, line, (DWORD)n, &w, NULL);
  printf("WriteFile(peb hStdOutput) -> %d, %lu bytes\n", ok, w);
  SIZE_T sz = 4u << 20; unsigned char *p = VirtualAlloc(NULL, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  memset(p, 0xAB, sz);
  ok = VirtualFree(p + 65536, 1u << 20, MEM_DECOMMIT);
  unsigned char *q = VirtualAlloc(p + 65536, 1u << 20, MEM_COMMIT, PAGE_READWRITE);
  unsigned long nz = 0; for (SIZE_T i = 0; i < (1u << 20); i++) nz += q[i] != 0;
  printf("decommit -> %d, recommit %s, nonzero after recommit: %lu, untouched byte: %02x\n", ok, q == p + 65536 ? "same" : "moved", nz, p[0]);
  return 0;
}
