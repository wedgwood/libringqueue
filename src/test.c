#include <assert.h>
#include <immintrin.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "ringqueue_lf.h"

static inline unsigned long tv2ms(const struct timeval *tv) {
  return ((unsigned)tv->tv_sec * 1000000 + tv->tv_usec) / 1000;
}

#define QLEN          (32 * 1024)
#define N             (QLEN * 1024)
#define ELE_EMPTY     0x0
#define ELE_FILLED    0XFEDCBA98

static int producers;
static int consumers;
static uint32_t *qtrace;
static int total_consumed = 0;

void test_1() {
  ringqueue_t *q = ringqueue_create(QLEN, 0);
  assert(q > 0);
  rq_lf_sess_t *sess = rq_lf_sess_create(q, 4, 4);
  assert(sess > 0);

  rq_lf_producer_t producer;
  rq_lf_producer_attach(&producer, sess);
  rq_lf_consumer_t consumer;
  rq_lf_consumer_attach(&consumer, sess);

  char buf[32];

  for (int i = 0; i < 10000; ++i) {
    int len = sprintf(buf, "data-%d", i);
    assert(len > 0);

    for (int j = 0; j < 1; ++j) {
      int rc = rq_lf_push(&producer, buf, len + 1);

      if (rc != 0) {
        printf("\nfailed: %d\n", rc);
        break;
      }
    }

    for (int j = 0; j < 1; ++j) {
      int rc = rq_lf_pop(&consumer, buf, sizeof(buf));

      if (rc > 0) {
        printf("\npop : %s\n", buf);
        assert(buf[0] == 'd');
      } else {
        printf("\nfailed: %d\n", rc);
        break;
      }
    }
  }

  rq_lf_producer_detach(&producer);
  rq_lf_consumer_detach(&consumer);
  rq_lf_sess_destroy(sess);
  ringqueue_destroy(q);
}

void *pt_producer(void *arg) {
  rq_lf_producer_t *p = (rq_lf_producer_t *)arg;
  int thid = p->id;
  printf("\nproducer : %d\n", thid);
  fflush(stdout);

  char buf[32];

  for (int i = thid; i < N * producers;) {
    uint32_t ele = i;
    /* int len = sprintf(buf, "data-%d", i); */
    assert(qtrace[i] == ELE_EMPTY);
    qtrace[i] = ELE_FILLED;

    do {
      int rc = rq_lf_push(p, (char *)&ele, sizeof(ele));

      if (rc) {
        assert(rc == RINGQUEUE_NOMEM);
        _mm_pause();
      } else {
        i += producers;
        break;
      }
    } while (1);
  }

  printf("\nproducer : %d finish!\n", thid);
  fflush(stdout);
  return NULL;
}

void *pt_consumer(void *arg) {
  uint32_t ele;
  rq_lf_consumer_t *c = (rq_lf_consumer_t *)arg;
  int thid = c->id;
  printf("\nconsumer : %d\n", thid);
  fflush(stdout);

  while (__sync_fetch_and_add(&total_consumed, 1) < N * producers) {
    do {
      int rc = rq_lf_pop(c, (char *)&ele, sizeof(ele));

      if (rc > 0) {
        assert(rc == 4);
        if (qtrace[ele] != ELE_FILLED) {
          printf("debug : ele -> %u, %u", ele, qtrace[ele]);
          assert(0);
          /* assert(qtrace[ele] == ELE_FILLED); */
        }

        break;
      } else {
        if (rc == RINGQUEUE_NOMEM) {
          /* assert(rc == 0); */
        }

        _mm_pause();
      }
    } while (1);
  }

  printf("\nconsumer : %d finish!\n", thid);
  fflush(stdout);

  return NULL;
}

void test_2() {
  ringqueue_t *q = ringqueue_create(QLEN, 0);
  assert(q > 0);
  qtrace = (uint32_t *)malloc(sizeof(uint32_t) * producers * N);

  memset(qtrace, 0, sizeof(uint32_t) * producers * N);

  /* for (int i = 0; i < producers * N; ++i) { */
    /* qtrace[i] = ELE_EMPTY; */
  /* } */

  rq_lf_sess_t *sess = rq_lf_sess_create(q, 4, 4);
  assert(sess > 0);

  rq_lf_producer_t producer[producers];
  rq_lf_consumer_t consumer[consumers];
  pthread_t tp[producers];
  pthread_t tc[consumers];

  struct timeval tv0, tv1;
  gettimeofday(&tv0, NULL);

  for (int i = 0; i < producers; ++i) {
    rq_lf_producer_attach(producer + i, sess);
    pthread_create(tp + i, NULL, pt_producer, producer + i);
  }

  usleep(10 * 1000);

  for (int i = 0; i < consumers; ++i) {
    rq_lf_consumer_attach(consumer + i, sess);
    pthread_create(tc + i, NULL, pt_consumer, consumer + i);
  }

  for (int i = 0; i < consumers; ++i) {
    while (pthread_join(tc[i], NULL)) {}
    rq_lf_consumer_detach(consumer + i);
  }

  for (int i = 0; i < producers; ++i) {
    while (pthread_join(tp[i], NULL)) {}
    rq_lf_producer_detach(producer + i);
  }

  gettimeofday(&tv1, NULL);
  printf("\ntime usage : %lu\n", tv2ms(&tv1) - tv2ms(&tv0));
  printf("\nspeed : %lu\n", N / ((tv2ms(&tv1) - tv2ms(&tv0))) * 1000);
  rq_lf_sess_destroy(sess);
  ringqueue_destroy(q);
}

int main(int argc, const char *argv[]) {
  if (argc < 3) {
    producers = consumers = 2;
  } else {
    producers = atoi(argv[1]);
    consumers = atoi(argv[2]);
  }

  test_2();
  return 0;
}
