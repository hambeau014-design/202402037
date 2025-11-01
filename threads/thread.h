#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* Thread identifier type. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)

/* Thread priorities. */
#define PRI_MIN 0
#define PRI_DEFAULT 31
#define PRI_MAX 63

/* Simplified MLFQS queue levels. */
enum mlfqs_queue { Q0 = 0, Q1 = 1, Q2 = 2 };

/* States. */
enum thread_status {
  THREAD_RUNNING,
  THREAD_READY,
  THREAD_BLOCKED,
  THREAD_DYING
};

typedef void thread_func (void *aux);
typedef void thread_action_func (struct thread *t, void *aux);

struct lock; /* fwd */

/* Thread control block. */
struct thread {
  /* Owned by thread.c. */
  tid_t tid;
  enum thread_status status;
  char name[16];
  uint8_t *stack;

  /* Priority scheduling (effective). */
  int priority;
  /* Original (base) priority for donation. */
  int original_priority;

  /* Donation list and link in other's donation list. */
  struct list donations;             /* of struct thread, via donation_elem */
  struct list_elem donation_elem;    /* when I am inside someone’s donations */
  struct lock *wait_on_lock;         /* lock I'm waiting on (donate target) */

  /* Ready/sleep/all list linkage. */
  struct list_elem elem;

  /* FIFO for equal-priority round-robin: increasing stamp when enqueued. */
  int64_t ready_stamp;

  /* Aging (in ready queues). */
  int age;

  /* Sleep support. */
  int64_t wakeup_tick;

  /* Simplified MLFQS (enabled when thread_mlfqs == true). */
  enum mlfqs_queue qlevel;   /* Q0→Q1→Q2 */
  int run_ticks_in_level;    /* used time slice inside current level */

#ifdef USERPROG
  uint32_t *pagedir;
#endif
  unsigned magic;
};

/* Global flag (set by kernel cmdline -mlfqs). */
extern bool thread_mlfqs;

/* Public API */
void thread_init (void);
void thread_start (void);
void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *aux);

void thread_block (void);
void thread_unblock (struct thread *);
void thread_yield (void);

const char *thread_name (void);
struct thread *thread_current (void);
tid_t thread_tid (void);
void thread_exit (void) NO_RETURN;

void thread_foreach (thread_action_func *, void *aux);

/* Priority APIs */
void thread_set_priority (int);
int  thread_get_priority (void);

/* Donation helpers */
void thread_update_priority (struct thread *t);
void thread_donate_priority (void);
void thread_remove_donation (struct lock *lock);

/* Sleep helper used by timer.c */
void thread_sleep_until (int64_t wake_tick);

#endif /* threads/thread.h */
