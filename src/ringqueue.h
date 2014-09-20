#ifndef __RINGQUEUE_H__
#define __RINGQUEUE_H__

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

typedef struct {
  uint64_t head;
  uint64_t tail;
  uint32_t mask;
  uint32_t len;
  uint8_t flag;
  char data[0];
} ringqueue_t;

#define RINGQUEUE_USE_SHM 0x01
#define RINGQUEUE_NOMEM -1

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

static inline uint32_t __to2pow(uint32_t hint) {
  uint32_t p = 0;

  while (hint >>= 1) {
    ++p;
  }

  return 2 << p;
}

static inline ringqueue_t *ringqueue_create(size_t hint, int flag) {
  ringqueue_t *ret = NULL;
  ringqueue_t *q   = NULL;
  uint32_t len     = __to2pow(hint);
  uint32_t mask    = len - 1;

  if (flag & RINGQUEUE_USE_SHM) {
    // TODO: shared mem
  } else {
    q = (ringqueue_t *)malloc(sizeof(ringqueue_t) + len);
  }

  if (q) {
    q->len  = len;
    q->mask = mask;
    q->head = q->tail = 0;
    q->flag = flag;
    ret     = q;
  }

  return ret;
}

static inline int ringqueue_destroy(ringqueue_t *q) {
  if (q->flag & RINGQUEUE_USE_SHM) {
    // TODO: shared mem
  } else {
    free(q);
  }

  return 0;
}

#endif //!__RINGQUEUE_H__
