const char* usage = "./usage:\n"
                    "./test-multi all\n"
                    "dest=127.0.0.1 stress_thread=1 ./test-multi client\n"
                    "stress_thread=1 io_thread=1 ./test-multi server\n";
#include "interface/group.h"
#include "r0/futex.h"
#include "r0/atomic.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
static void thread_counter_reg();
static int64_t thread_counter_sum(int64_t* addr);
extern int64_t rk_get_corse_us();

int grp = 1;
struct sockaddr_in dest_addr;
__thread int64_t handle_cnt;
__thread int64_t cb_cnt;

int serve_cb(int grp, const char* b, int64_t sz, uint64_t req_id)
{
  handle_cnt++;
  pn_resp(req_id, b, sz);
  return 0;
}

int client_cb(void* arg, int error, const char* b, int64_t sz)
{
  cb_cnt++;
  int* cond = (typeof(cond))arg;
  if (cond) {
    (*cond)++;
    rk_futex_wake(cond, 1);
  }
  return 0;
}

void* stress_thread(void* arg)
{
  int64_t tid = (int64_t)arg;
  int cond = 0;
  thread_counter_reg();
  prctl(PR_SET_NAME, "stress");
  srand((unsigned)time(NULL));
  for(int i = 0; ;i++) {
    int msg_length = 16 + rand() % 4080;
#ifndef LARGEPACKET_ERRSIM_FREQ
#define LARGEPACKET_ERRSIM_FREQ 10
#endif
#ifdef PNIO_ERRSIM
  if (rand() % LARGEPACKET_ERRSIM_FREQ == 0) {
    msg_length = 8*1024*1024 + rand()%(8*1024*1024);
  }
#endif
    char *msg = (char*)malloc(msg_length);
    int* pcond = (i & 0xff)? NULL: &cond;
    pn_send(((int64_t)grp<<32) + tid, &dest_addr, msg, msg_length, 0, rk_get_corse_us() + 10000000, client_cb, pcond);
    free(msg);
    usleep(0.005*1000*1000);
    if (pcond) {
      int cur_cond = 0;
      while((cur_cond = LOAD(pcond)) < ((i>>8) - 64)) {
        rk_futex_wait(pcond, cur_cond, NULL);
      }
    }
  }
}

void report();
static struct sockaddr_in* rk_make_unix_sockaddr(struct sockaddr_in *sin, in_addr_t ip, int port);
#define cfg(k, v) (getenv(k)?:v)
#define cfgi(k, v) atoi(getenv(k)?:v)
#define streq(s1, s2) (0 == strcmp(s1, s2))
int main(int argc, char** argv)
{
#ifdef PNIO_ERRSIM
  srand((unsigned)time(NULL));
#endif
  if (argc != 2) {
      fprintf(stderr, "%s\n", usage);
      return 1;
  }
  signal(SIGPIPE, SIG_IGN);
  const char* mode = argv[1];
  int lfd = -1;
  if (streq(mode, "server") || streq(mode, "all")) {
    lfd = pn_listen(8042, serve_cb);
  }
  int cnt = pn_provision(lfd, grp, cfgi("io_thread", "1"));
  if (cnt != cfgi("io_thread", "1")) {
    printf("pn_provision failed, cnt = %d", cnt);
    if (cnt <= 0) {
      exit(-1);
    }
  }
  rk_make_unix_sockaddr(&dest_addr, inet_addr(cfg("dest", "127.0.0.1")), 8042);
  if (streq(mode, "client") || streq(mode, "all")) {
    int stress_count = cfgi("stress_thread", "1");
    pthread_t thd[1024];
    for(int64_t i = 0; i < stress_count; i++) {
      pthread_create(thd +i, NULL, stress_thread, (void*)i);
    }
  }
  report();
  return 0;
}

#include "pkt-nio.c"

void report()
{
  int64_t last_handle_cnt = 0;
  int64_t last_cb_cnt = 0;
  format_t f;
  format_init(&f, sizeof(f.buf));
  while(1) {
    format_reset(&f);
    mod_report(&f);
    int64_t cur_cb_cnt = thread_counter_sum(&cb_cnt);
    int64_t cur_handle_cnt = thread_counter_sum(&handle_cnt);
    printf("handle: %ld cb: %ld alloc: %s\n", cur_handle_cnt - last_handle_cnt, cur_cb_cnt - last_cb_cnt, format_gets(&f));
    last_handle_cnt = cur_handle_cnt;
    last_cb_cnt = cur_cb_cnt;
    usleep(1000 * 1000);
  }
}
