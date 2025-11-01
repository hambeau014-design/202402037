#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b
#define TIME_SLICE   4
#define AGING_TICKS  20      /* age threshold to raise priority or promote */

/* Non-MLFQS ready queue (priority-ordered, FIFO among equals). */
static struct list ready_list;
/* Simplified MLFQS run queues when thread_mlfqs == true. */
static struct list rq_q0, rq_q1, rq_q2;

static struct list all_list;

/* Sleepers (sorted by wakeup_tick ascending). */
static struct list sleep_list;
static int64_t next_wakeup = INT64_MAX;

/* Idle/initial */
static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

/* Stats */
static long long idle_ticks, kernel_ticks, user_ticks;
static unsigned thread_ticks;

/* global flags */
bool thread_mlfqs = false;

/* FIFO tie-breaker */
static int64_t ready_counter = 0;

/* Slices for simplified MLFQS */
#define SLICE_Q0 2
#define SLICE_Q1 4
#define SLICE_Q2 8

/* Prototypes */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* -------- comparators -------- */

/* ready_list ordering: priority desc, FIFO among equals by ready_stamp asc */
static bool cmp_ready_fifo (const struct list_elem *a,
                            const struct list_elem *b,
                            void *aux UNUSED) {
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  if (ta->priority != tb->priority) return ta->priority > tb->priority;
  return ta->ready_stamp < tb->ready_stamp;
}

/* sleepers by wakeup_tick asc */
static bool cmp_wakeup (const struct list_elem *a,
                        const struct list_elem *b,
                        void *aux UNUSED) {
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->wakeup_tick < tb->wakeup_tick;
}

/* -------- internals -------- */

static void sleepers_try_wake (int64_t now_ticks) {
  while (!list_empty (&sleep_list)) {
    struct thread *t = list_entry (list_front (&sleep_list), struct thread, elem);
    if (t->wakeup_tick > now_ticks) break;
    list_pop_front (&sleep_list);
    thread_unblock (t);
  }
  next_wakeup = list_empty (&sleep_list) ?
                INT64_MAX :
                list_entry (list_front (&sleep_list), struct thread, elem)->wakeup_tick;
}

static inline struct list *mlfqs_queue_of (enum mlfqs_queue q) {
  return (q == Q0) ? &rq_q0 : (q == Q1) ? &rq_q1 : &rq_q2;
}

static int current_level_slice (enum mlfqs_queue q) {
  return (q == Q0) ? SLICE_Q0 : (q == Q1) ? SLICE_Q1 : SLICE_Q2;
}

/* -------- init -------- */
void thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&rq_q0);
  list_init (&rq_q1);
  list_init (&rq_q2);

  list_init (&all_list);
  list_init (&sleep_list);

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

void thread_start (void)
{
  idle_thread = thread_create ("idle", PRI_MIN, idle, NULL);
  intr_enable ();
}

/* -------- tick -------- */
void thread_tick (void)
{
  struct thread *cur = thread_current ();
  int64_t now = timer_ticks ();

  /* wake up sleeping threads if it is time */
  if (next_wakeup <= now)
    sleepers_try_wake (now);

  /* stats */
  if (cur == idle_thread) idle_ticks++;
#ifdef USERPROG
  else if (cur->pagedir != NULL) user_ticks++;
#endif
  else kernel_ticks++;

  /* RR: time slice for non-MLFQS */
  if (!thread_mlfqs) {
    /* aging on ready_list */
    bool need_preempt = false;
    for (struct list_elem *e = list_begin (&ready_list);
         e != list_end (&ready_list); ) {
      struct thread *t = list_entry (e, struct thread, elem);
      t->age++;
      if (t->age >= AGING_TICKS && t->priority < PRI_DEFAULT) {
        t->age = 0;
        t->priority++;
        struct list_elem *next = list_next (e);
        list_remove (&t->elem);
        t->ready_stamp = ++ready_counter;
        list_insert_ordered (&ready_list, &t->elem, cmp_ready_fifo, NULL);
        e = next;
        /* top could change; if higher than current, preempt */
        need_preempt = true;
        continue;
      }
      e = list_next (e);
    }

    if (++thread_ticks >= TIME_SLICE) {
      thread_ticks = 0;
      intr_yield_on_return ();
      return;
    }

    if (need_preempt && !list_empty (&ready_list)) {
      struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
      if (top->priority > cur->priority)
        intr_yield_on_return ();
    }
  } else {
    /* Simplified MLFQS: three queues Q0/Q1/Q2 */
    cur->run_ticks_in_level++;
    int slice = current_level_slice (cur->qlevel);

    /* aging for waiting threads in all queues; promote when age hits threshold */
    bool promoted_to_higher = false;
    for (int q = Q2; q >= Q0; q--) {
      struct list *l = mlfqs_queue_of ((enum mlfqs_queue)q);
      for (struct list_elem *e = list_begin (l); e != list_end (l); ) {
        struct thread *t = list_entry (e, struct thread, elem);
        t->age++;
        if (t->age >= AGING_TICKS && t->qlevel > Q0) {
          t->age = 0;
          enum mlfqs_queue newq = (enum mlfqs_queue)(t->qlevel - 1);
          struct list_elem *next = list_next (e);
          list_remove (&t->elem);
          t->qlevel = newq;
          t->ready_stamp = ++ready_counter;
          list_push_back (mlfqs_queue_of (t->qlevel), &t->elem);
          e = next;
          if (t->qlevel == Q0) promoted_to_higher = true;
          continue;
        }
        e = list_next (e);
      }
    }

    /* if Q1/Q2 running and Q0 is not empty now, preempt */
    if ((cur->qlevel != Q0) && !list_empty (&rq_q0))
      intr_yield_on_return ();

    /* demote when consuming full slice */
    if (cur != idle_thread && cur->run_ticks_in_level >= slice) {
      cur->run_ticks_in_level = 0;
      if (cur->qlevel < Q2) cur->qlevel = (enum mlfqs_queue)(cur->qlevel + 1);
      thread_yield ();
      return;
    }

    if (promoted_to_higher && cur->qlevel != Q0 && !list_empty (&rq_q0))
      intr_yield_on_return ();
  }
}

/* -------- stats -------- */
void thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* -------- create -------- */
tid_t thread_create (const char *name, int priority, thread_func *function, void *aux)
{
  struct thread *t;
  tid_t tid;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL) return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  struct kernel_thread_frame *kf = (struct kernel_thread_frame *) ((uint8_t *) t + PGSIZE - sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  struct switch_entry_frame *ef = (struct switch_entry_frame *) ((uint8_t *) kf - sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  struct switch_threads_frame *sf = (struct switch_threads_frame *) ((uint8_t *) ef - sizeof *sf);
  sf->eip = (void (*) (void)) switch_entry;
  sf->ebp = 0;

  enum intr_level old = intr_disable ();
  thread_unblock (t);
  intr_set_level (old);

  if (!thread_mlfqs && t->priority > thread_current ()->priority)
    thread_yield ();
  if (thread_mlfqs && thread_current ()->qlevel != Q0 && !list_empty (&rq_q0))
    thread_yield ();

  return tid;
}

/* -------- block / unblock / yield -------- */
void thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);
  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

static void enqueue_ready (struct thread *t) {
  t->age = 0;
  t->ready_stamp = ++ready_counter;
  if (thread_mlfqs) {
    list_push_back (mlfqs_queue_of (t->qlevel), &t->elem);
  } else {
    list_insert_ordered (&ready_list, &t->elem, cmp_ready_fifo, NULL);
  }
}

void
thread_unblock (struct thread *t)
{
  enum intr_level old_level = intr_disable ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_BLOCKED);

  enqueue_ready (t);
  t->status = THREAD_READY;

  /* 즉시 선점: 무조건 intr_yield_on_return() 사용 */
  if (t != thread_current () && t->priority > thread_current ()->priority)
    intr_yield_on_return ();

  intr_set_level (old_level);
}

void thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old = intr_disable ();
  if (cur != idle_thread) {
    enqueue_ready (cur);
    cur->status = THREAD_READY;
  }
  schedule ();
  intr_set_level (old);
}

/* -------- sleep -------- */
void thread_sleep_until (int64_t wake_tick)
{
  enum intr_level old = intr_disable ();
  struct thread *cur = thread_current ();
  ASSERT (cur != idle_thread);

  cur->wakeup_tick = wake_tick;
  list_insert_ordered (&sleep_list, &cur->elem, cmp_wakeup, NULL);
  if (wake_tick < next_wakeup) next_wakeup = wake_tick;

  thread_block ();
  intr_set_level (old);
}

/* -------- getters -------- */
const char *thread_name (void) { return thread_current ()->name; }
struct thread *thread_current (void) { return running_thread (); }
tid_t thread_tid (void) { return thread_current ()->tid; }

/* -------- exit -------- */
void thread_exit (void)
{
  ASSERT (!intr_context ());
#ifdef USERPROG
  process_exit ();
#endif
  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* -------- priority API -------- */
void thread_set_priority (int new_priority)
{
  enum intr_level old = intr_disable ();
  struct thread *cur = thread_current ();
  cur->original_priority = new_priority;
  thread_update_priority (cur);

  if (!thread_mlfqs) {
    if (!list_empty (&ready_list)) {
      struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
      if (top->priority > cur->priority)
        thread_yield ();
    }
  } else {
    /* In MLFQS mode, priority value is not used for ordering queues,
       but we still allow yield if a Q0 exists while running Q1/Q2. */
    if (cur->qlevel != Q0 && !list_empty (&rq_q0))
      thread_yield ();
  }

  intr_set_level (old);
}

int thread_get_priority (void) { return thread_current ()->priority; }

/* -------- donation helpers -------- */
void thread_update_priority (struct thread *t)
{
  int maxp = t->original_priority;
  if (!list_empty (&t->donations)) {
    struct thread *donor = list_entry (list_front (&t->donations), struct thread, donation_elem);
    if (donor->priority > maxp) maxp = donor->priority;
  }
  t->priority = maxp;
}

void thread_donate_priority (void)
{
  struct thread *cur = thread_current ();
  for (int depth = 0; depth < 8; depth++) {
    struct lock *lk = cur->wait_on_lock;
    if (!lk || !lk->holder) break;
    struct thread *holder = lk->holder;
    if (holder->priority < cur->priority)
      holder->priority = cur->priority;
    cur = holder;
  }
}

void thread_remove_donation (struct lock *lock)
{
  struct thread *cur = thread_current ();
  for (struct list_elem *e = list_begin (&cur->donations);
       e != list_end (&cur->donations); ) {
    struct thread *t = list_entry (e, struct thread, donation_elem);
    if (t->wait_on_lock == lock)
      e = list_remove (&t->donation_elem);
    else
      e = list_next (e);
  }
  thread_update_priority (cur);
}

/* -------- scheduler core -------- */
static struct thread *next_thread_to_run (void)
{
  if (!thread_mlfqs) {
    if (list_empty (&ready_list)) return idle_thread;
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
  } else {
    if (!list_empty (&rq_q0)) return list_entry (list_pop_front (&rq_q0), struct thread, elem);
    if (!list_empty (&rq_q1)) return list_entry (list_pop_front (&rq_q1), struct thread, elem);
    if (!list_empty (&rq_q2)) return list_entry (list_pop_front (&rq_q2), struct thread, elem);
    return idle_thread;
  }
}

static void schedule (void)
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

void thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
#ifdef USERPROG
  process_activate ();
#endif
  if (prev != NULL && prev->status == THREAD_DYING)
    palloc_free_page (prev);

  /* reset run_ticks_in_level when a thread starts to run */
  cur->run_ticks_in_level = 0;
}

/* -------- init helpers -------- */
static void init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->original_priority = priority;
  list_init (&t->donations);
  t->wait_on_lock = NULL;

  t->age = 0;
  t->ready_stamp = 0;
  t->wakeup_tick = 0;

  t->qlevel = Q0;              /* MLFQS starts in Q0 by spec */
  t->run_ticks_in_level = 0;

  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

static struct thread *running_thread (void)
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) ((uintptr_t) esp & ~(PGSIZE - 1));
}

static tid_t allocate_tid (void)
{
  static tid_t next_tid = 1;
  tid_t tid;
  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);
  return tid;
}

/* -------- kernel/user helpers -------- */
static void kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);
  intr_enable ();
  function (aux);
  thread_exit ();
}

static void idle (void *aux UNUSED)
{
  for (;;) {
    enum intr_level old = intr_disable ();
    if (list_empty (&ready_list) &&
        list_empty (&rq_q0) && list_empty (&rq_q1) && list_empty (&rq_q2))
      asm volatile ("sti; hlt" : : : "memory");
    intr_set_level (old);
    thread_yield ();
  }
}
