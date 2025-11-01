#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>
#include "threads/thread.h"

/* Counting semaphore. */
struct semaphore {
  unsigned value;
  struct list waiters;   /* of struct thread via thread.elem (priority/FIFO) */
};

/* Lock with priority donation. */
struct lock {
  struct thread *holder;
  struct semaphore semaphore;  /* binary semaphore */
};

/* Condition variable. */
struct condition {
  struct list waiters;         /* of struct semaphore_elem (see synch.c) */
};

/* Semaphore API */
void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

/* Lock API */
void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/* Condition variable API */
void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

#endif /* threads/synch.h */
