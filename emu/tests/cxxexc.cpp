#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
struct Guard { const char *n; Guard(const char *n) : n(n) { printf("  ctor %s\n", n); } ~Guard() { printf("  dtor %s\n", n); } };
static int depth3(int v) { Guard g("depth3"); if (v > 2) throw std::runtime_error("value too large: " + std::to_string(v)); return v * 2; }
static int depth2(int v) { Guard g("depth2"); return depth3(v) + 1; }
static int depth1(int v) { Guard g("depth1"); try { return depth2(v); } catch (const std::logic_error &) { puts("  wrong catch"); return -1; } }
struct MyErr { int code; };
int main() {
  int total = 0;
  try { total += depth1(1); printf("no throw: %d\n", total); } catch (...) { puts("unexpected"); }
  try { total += depth1(7); puts("not reached"); } catch (const std::exception &e) { printf("caught std::exception: %s\n", e.what()); }
  try { throw MyErr{42}; } catch (const MyErr &m) { printf("caught MyErr %d\n", m.code); }
  try { try { throw std::out_of_range("inner"); } catch (const std::exception &e) { printf("inner caught %s, rethrowing as int\n", e.what()); throw 5; } }
  catch (int i) { printf("outer caught int %d\n", i); }
  try { std::vector<int> v(3); v.at(10) = 1; } catch (const std::out_of_range &e) { printf("vector::at threw: %s\n", e.what()); }
  try { std::string s = std::to_string(std::stoi("12x")); printf("stoi=%s\n", s.c_str()); } catch (const std::invalid_argument &e) { printf("stoi ok: %s\n", e.what()); }
  for (int i = 0; i < 1000; i++) { try { if (i & 1) throw i; } catch (int) { total++; } }
  printf("1000 throws in a loop, total=%d\n", total);
  printf("done\n");
  return 0;
}
