/* mingw-w64 winpthreads test (zig's mingw lacks C11 <threads.h>) for the Nebula x64 emulator.
   Build: zig cc -target x86_64-windows-gnu -O2 -o pthreads.exe pthreads.c -lpthread */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_fail, g_n;
#define CHECK(cond, name) do { g_n++; if (cond) printf("ok %d %s\n", g_n, name); else { g_fail++; printf("FAIL %d %s\n", g_n, name); } fflush(stdout); } while (0)

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER; static int g_counter;
static void *inc(void *a) { for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&g_m); int v = g_counter; if ((i & 63) == 0) sched_yield(); g_counter = v + 1; pthread_mutex_unlock(&g_m); } return (void *)(size_t)(42 + (int)(size_t)a); }

static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER; static int g_items, g_produced, g_consumed, g_stop;
static void *producer(void *a) { for (int i = 0; i < 300; i++) { pthread_mutex_lock(&g_m); g_items++; g_produced++; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_m); if ((i & 15) == 0) sched_yield(); } pthread_mutex_lock(&g_m); g_stop = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_m); return 0; }
static void *consumer(void *a) { int n = 0; for (;;) { pthread_mutex_lock(&g_m); while (!g_items && !g_stop) pthread_cond_wait(&g_cv, &g_m); if (!g_items && g_stop) { pthread_mutex_unlock(&g_m); break; } g_items--; g_consumed++; n++; pthread_mutex_unlock(&g_m); } return (void *)(size_t)n; }

static sem_t g_sem; static void *poster(void *a) { Sleep(15); sem_post(&g_sem); sem_post(&g_sem); return 0; }

static pthread_once_t g_once = PTHREAD_ONCE_INIT; static int g_once_calls; static void once_fn(void) { g_once_calls++; }
static void *oncer(void *a) { pthread_once(&g_once, once_fn); return 0; }

static pthread_key_t g_key; static __thread int g_tl = 3;
static void *keyer(void *a) { pthread_setspecific(g_key, a); g_tl += (int)(size_t)a; for (int i = 0; i < 20; i++) sched_yield(); return (void *)(size_t)(pthread_getspecific(g_key) == a && g_tl == 3 + (int)(size_t)a); }

static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER; static volatile LONG g_readers;
static void *rdr(void *a) { pthread_rwlock_rdlock(&g_rw); InterlockedIncrement(&g_readers); Sleep(20); pthread_rwlock_unlock(&g_rw); return 0; }

int main(void) {
  pthread_t t[8]; void *r;
  for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, inc, (void *)(size_t)i);
  int codes = 1; for (int i = 0; i < 4; i++) { pthread_join(t[i], &r); if ((int)(size_t)r != 42 + i) codes = 0; }
  CHECK(g_counter == 4000, "pthread_mutex serialises 4 threads (4000)");
  CHECK(codes, "pthread_join returns each thread's value");

  pthread_create(&t[0], NULL, consumer, 0); pthread_create(&t[1], NULL, consumer, 0); pthread_create(&t[2], NULL, producer, 0);
  void *n0, *n1; pthread_join(t[2], &r); pthread_join(t[0], &n0); pthread_join(t[1], &n1);
  CHECK(g_consumed == 300 && (int)(size_t)n0 + (int)(size_t)n1 == 300, "pthread_cond_wait/signal producer-consumer (300 items)");

  sem_init(&g_sem, 0, 0); pthread_create(&t[0], NULL, poster, 0);
  CHECK(sem_trywait(&g_sem) != 0, "sem_trywait on 0 fails");
  sem_wait(&g_sem); sem_wait(&g_sem); int sv = -1; sem_getvalue(&g_sem, &sv);
  CHECK(sv == 0, "sem_wait consumed both posts"); pthread_join(t[0], 0); sem_destroy(&g_sem);

  for (int i = 0; i < 5; i++) pthread_create(&t[i], NULL, oncer, 0); for (int i = 0; i < 5; i++) pthread_join(t[i], 0);
  CHECK(g_once_calls == 1, "pthread_once runs once across 5 threads");

  pthread_key_create(&g_key, NULL); pthread_setspecific(g_key, (void *)77);
  for (int i = 0; i < 5; i++) pthread_create(&t[i], NULL, keyer, (void *)(size_t)(i + 1));
  int tls_ok = 1; for (int i = 0; i < 5; i++) { pthread_join(t[i], &r); if (!r) tls_ok = 0; }
  CHECK(tls_ok && pthread_getspecific(g_key) == (void *)77 && g_tl == 3, "pthread_key + __thread isolation");

  g_readers = 0; for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, rdr, 0);
  Sleep(5); pthread_rwlock_wrlock(&g_rw);
  CHECK(g_readers == 3, "rwlock wrlock waits for 3 readers");
  pthread_rwlock_unlock(&g_rw); for (int i = 0; i < 3; i++) pthread_join(t[i], 0);

  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50 * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
  pthread_mutex_lock(&g_m); DWORD t0 = GetTickCount(); int rc = pthread_cond_timedwait(&g_cv, &g_m, &ts); DWORD dt = GetTickCount() - t0; pthread_mutex_unlock(&g_m);
  CHECK(rc == ETIMEDOUT && dt >= 30, "pthread_cond_timedwait times out (ETIMEDOUT)");

  printf("%d checks, %d failed\n", g_n, g_fail);
  return g_fail;
}
