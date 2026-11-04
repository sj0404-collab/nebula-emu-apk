#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
__attribute__((noinline)) long double ldmul(long double a, long double b) { return a * b; }
__attribute__((noinline)) long double ldsqrt(long double a) { return sqrtl(a); }
int main(void) {
  long double a = 3.0L, b = 7.0L; long double r = ldmul(a, b) / 11.0L;
  printf("ld: %.10Lf %d\n", r, (int)(r * 1000));
  long double s = ldsqrt(2.0L); printf("sqrtl: %.9Lf\n", s);
  double d = 123456.789; printf("%%f via mingw: %f %.3e %g\n", d, d, 1e-5);
  long double big = 1e300L; printf("cmp: %d %d\n", big > 1e299L, r < 0.0L);
  long long ll = (long long)(r * 1e6L); printf("fistp: %lld\n", ll);
  printf("fmodl: %.4Lf sinl: %.6Lf\n", fmodl(10.5L, 3.0L), sinl(1.0L));
  char *e; printf("strtold: %.6Lf\n", strtold("2.718281828", &e));
  return 0;
}
