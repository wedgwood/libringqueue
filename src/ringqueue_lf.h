#ifndef __RINGQUEUE_LF_H__
#define __RINGQUEUE_LF_H__

#include "ringqueue.h"

#include <immintrin.h>
#include <string.h>
// #include <sys/mman.h>

typedef struct {
  uint64_t idx;
  uint8_t attached;
} rq_lf_sess_entry_t;

// last_head
//   |
// head
//   |
// last_tail
//   |
// tail

typedef struct {
  ringqueue_t *q;
  rq_lf_sess_entry_t *c;
  rq_lf_sess_entry_t *p;
  uint64_t last_head;
  uint64_t last_tail;
  uint32_t nc;
  uint32_t np;
} rq_lf_sess_t;

// it's not easy allow any more worker dynamic attach the queue
// fix size workers may be enough
static inline rq_lf_sess_t *rq_lf_sess_create(ringqueue_t *q, int nc, int np) {
  rq_lf_sess_t *ret  = NULL;
  rq_lf_sess_t *sess = NULL;

  if (q->flag & RINGQUEUE_USE_SHM) {
    // TODO: shared mem
  } else {
    sess = (rq_lf_sess_t *)calloc(sizeof(rq_lf_sess_t) + sizeof(rq_lf_sess_entry_t) * (nc + np), 1);
  }

  if (sess) {
    sess->last_head = sess->last_tail = 0;
    sess->nc = nc;
    sess->np = np;
    sess->c  = (rq_lf_sess_entry_t *)((char *)sess + sizeof(rq_lf_sess_t));
    sess->p  = (rq_lf_sess_entry_t *)((char *)sess + sizeof(rq_lf_sess_t)) + nc;

    for (int i = 0; i < nc; ++i) {
      sess->c[i].idx = UINT64_MAX;
    }

    for (int i = 0; i < np; ++i) {
      sess->p[i].idx = UINT64_MAX;
    }

    sess->q  = q;
    ret = sess;
  }

  return ret;
}

static inline int rq_lf_sess_destroy(rq_lf_sess_t *sess) {
  if (sess->q->flag & RINGQUEUE_USE_SHM) {

  } else {
    free(sess);
  }

  return 0;
}

typedef struct {
  rq_lf_sess_t *sess;
  uint64_t head;
  uint32_t id;
} rq_lf_consumer_t;

typedef struct {
  rq_lf_sess_t *sess;
  uint64_t tail;
  uint32_t id;
} rq_lf_producer_t;

static inline int rq_lf_consumer_attach(rq_lf_consumer_t *c, rq_lf_sess_t *sess) {
  int ret = -1;

  for (int i = 0; i < sess->nc; ++i) {
    if (!sess->c[i].attached) {
      if (__sync_bool_compare_and_swap(&sess->c[i].attached, 0, 1)) {
        c->id          = i;
        c->sess        = sess;
        sess->c[i].idx = UINT64_MAX;
        ret            = i;
        break;
      }
    }
  }

  return ret;
}

static inline void rq_lf_consumer_detach(rq_lf_consumer_t *c) {
  c->sess->c[c->id].attached = 0;
}

static inline int rq_lf_producer_attach(rq_lf_producer_t *p, rq_lf_sess_t *sess) {
  int ret = -1;

  for (int i = 0; i < sess->np; ++i) {
    if (!sess->p[i].attached) {
      if (__sync_bool_compare_and_swap(&sess->p[i].attached, 0, 1)) {
        p->id          = i;
        p->sess        = sess;
        sess->p[i].idx = UINT64_MAX;
        ret            = i;
        break;
      }
    }
  }

  return ret;
}

static inline void rq_lf_producer_detach(rq_lf_producer_t *p) {
  p->sess->p[p->id].attached = 0;
}

static inline ssize_t rq_lf_pop(rq_lf_consumer_t *c, char *buf, size_t sz) {
  int ret               = 0;
  rq_lf_sess_t *sess    = c->sess;
  rq_lf_sess_entry_t *e = sess->c + c->id;
  ringqueue_t *q        = sess->q;
  uint32_t len          = q->len;

  uint64_t head;
  uint64_t new_head;
  uint32_t real_head;
  uint32_t data_sz;

  do {
    e->idx           = q->head;
    head             = e->idx;
    real_head        = head & q->mask;
    uint32_t padding = 0;

    if (unlikely(head >= sess->last_tail)) {
      uint64_t last_tail = q->tail;

      for (int i = 0; i < sess->np; ++i) {
        if (sess->p[i].attached) {
          uint64_t idx = sess->p[i].idx;

          // asm volatile("" ::: "memory");

          if (idx < last_tail) {
            last_tail = idx;
          }
        }
      }

      sess->last_tail = last_tail;

      if (head >= last_tail) {
        break;
      }
    }

    if (unlikely(len < real_head + sizeof(uint32_t))) {
      padding   = len - real_head;
      real_head = 0;
    }

    // may get corrupt data, but the same time, cas of head will not succeed
    data_sz = *(uint32_t *)(q->data + real_head);

    if (data_sz > sz) {
      ret = RINGQUEUE_NOMEM;
      break;
    }

    new_head = head + padding + sizeof(uint32_t) + data_sz;

    if (!__sync_bool_compare_and_swap(&q->head, head, new_head)) {
      continue;
    }

    ret = data_sz;
    break;
  } while (1);

  if (ret > 0) {
    if (real_head) {
      uint32_t part1 = len - real_head - sizeof(uint32_t);

      if (part1 >= data_sz) {
        memcpy(buf, q->data + real_head + sizeof(uint32_t), data_sz);
      } else {
        if (part1 > 0) {
          memcpy(buf, q->data + real_head + sizeof(uint32_t), part1);
        }

        memcpy(buf + part1, q->data, data_sz - part1);
      }
    } else {
      memcpy(buf, q->data + sizeof(uint32_t), data_sz);
    }
  }

  asm volatile("" ::: "memory");
  e->idx = UINT64_MAX;
  return ret;
}

// push buf into queue, pack the data as '[sz][buf]'. padding will be used, if '[sz]' could be wrapped
static inline int rq_lf_push(rq_lf_producer_t *p, const char *buf, size_t sz) {
  int ret               = 0;
  rq_lf_sess_t *sess    = p->sess;
  rq_lf_sess_entry_t *e = sess->p + p->id;
  ringqueue_t *q        = sess->q;
  uint32_t len          = q->len;

  uint64_t tail;
  uint64_t new_tail;
  uint32_t real_tail;

  do {
    e->idx           = q->tail;
    tail             = e->idx;
    real_tail        = tail & q->mask;
    uint32_t padding = 0;

    if (unlikely(len - real_tail < sizeof(uint32_t))) {
      padding   = len - real_tail;
      real_tail = 0;
    }

    // assert(tail >= sess->last_head);

    if (unlikely(len < tail - sess->last_head + padding + sizeof(uint32_t) + sz)) {
      uint64_t last_head = q->head;

      for (int i = 0; i < sess->nc; ++i) {
        if (sess->c[i].attached) {
          uint64_t idx = sess->c[i].idx;

          // asm volatile("" ::: "memory");

          if (idx < last_head) {
            last_head = idx;
          }
        }
      }

      sess->last_head = last_head;

      if (len < tail - sess->last_head + padding + sizeof(uint32_t) + sz) {
        ret = RINGQUEUE_NOMEM;
        break;
      }
    }

    new_tail = tail + padding + sizeof(uint32_t) + sz;
  } while (!__sync_bool_compare_and_swap(&q->tail, tail, new_tail));

  if (!ret) {
    *(uint32_t *)(q->data + real_tail) = sz;

    if (real_tail) {
      uint32_t part1 = len - real_tail - sizeof(uint32_t);

      if (part1 >= sz) {
        memcpy(q->data + real_tail + sizeof(uint32_t), buf, sz);
      } else {
        if (part1 > 0) {
          memcpy(q->data + real_tail + sizeof(uint32_t), buf, part1);
        }

        memcpy(q->data, buf + part1, sz - part1);
      }
    } else {
      memcpy(q->data + sizeof(uint32_t), buf, sz);
    }
  }

  e->idx = UINT64_MAX;
  return ret;
}

#endif //!__RINGQUEUE_LF_H__
