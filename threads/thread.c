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
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b
#define TIME_SLICE 4

/* Ready list and all threads list. */
static struct list ready_list;
static struct list all_list;

/* Sleepers. */
static struct list sleep_list;
static int64_t next_tick_to_wakeup = INT64_MAX;

/* Idle thread. */
static struct thread *idle_thread;
/* Initial thread. */
static struct thread *initial_thread;
/* Thread identifier lock. */
static struct lock tid_lock;

/* Statistics. */
static long long idle_ticks, kernel_ticks, user_ticks;
static unsigned thread_ticks;

bool thread_mlfqs;

/* Prototypes. */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* --------- 비교 함수들 --------- */
static bool
cmp_ready (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority; /* 내림차순: 높은게 앞 */
}

static bool
cmp_donation (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  const struct thread *ta = list_entry (a, struct thread, donation_elem);
  const struct thread *tb = list_entry (b, struct thread, donation_elem);
  return ta->priority > tb->priority;
}

/* --------- Sleep helpers --------- */
static void
update_next_tick (int64_t tick) {
  if (tick < next_tick_to_wakeup) next_tick_to_wakeup = tick;
}

static bool
cmp_wakeup (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->wakeup_tick < tb->wakeup_tick;
}

/* --------- Core --------- */
void thread_init (void) {
  ASSERT (intr_get_level () == INTR_OFF);
  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

void thread_start (void) {
  idle_thread = thread_create ("idle", PRI_MIN, idle, NULL);
  intr_enable ();
}

void thread_tick (void) {
  struct thread *cur = thread_current ();
  /* wake sleepers */
  int64_t now = timer_ticks ();
  if (next_tick_to_wakeup <= now) {
    struct list_elem *e = list_begin (&sleep_list);
    while (e != list_end (&sleep_list)) {
      struct thread *t = list_entry (e, struct thread, elem);
      if (t->wakeup_tick <= now) {
        e = list_remove (&t->elem);
        thread_unblock (t);
      } else break;
    }
    /* recompute next */
    next_tick_to_wakeup = (list_empty(&sleep_list) ? INT64_MAX
                             : list_entry(list_front(&sleep_list), struct thread, elem)->wakeup_tick);
  }

  if (cur == idle_thread) idle_ticks++;
#ifdef USERPROG
  else if (cur->pagedir != NULL) user_ticks++;
#endif
  else kernel_ticks++;

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

tid_t thread_create (const char *name, int priority, thread_func *function, void *aux) {
  struct thread *t;
  tid_t tid;
  enum intr_level old = intr_disable ();

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL) {
    intr_set_level (old);
    return TID_ERROR;
  }

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  struct kernel_thread_frame *kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL; kf->function = function; kf->aux = aux;

  struct switch_entry_frame *ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  struct switch_threads_frame *sf = alloc_frame (t, sizeof *sf);
  sf->eip = (void (*) (void)) switch_entry; sf->ebp = 0;

  thread_unblock (t);
  intr_set_level (old);

  if (t->priority > thread_current ()->priority)
    thread_yield ();

  return tid;
}

void thread_block (void) {
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);
  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

void
thread_unblock (struct thread *t)
{
  enum intr_level old_level = intr_disable ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_BLOCKED);

  list_insert_ordered (&ready_list, &t->elem, cmp_ready, NULL);
  t->status = THREAD_READY;

  /*깨운 스레드가 현재보다 높으면 즉시 양보 */
  if (t != thread_current () && t->priority > thread_current ()->priority)
  {
    if (!intr_context ())
      thread_yield ();
    else
      intr_yield_on_return ();
  }

  intr_set_level (old_level);
}

/* Sleep API (timer_sleep에서 사용) */
void thread_sleep_until (int64_t wake_tick) {
  enum intr_level old = intr_disable ();
  struct thread *cur = thread_current ();
  ASSERT (cur != idle_thread);
  cur->wakeup_tick = wake_tick;
  list_insert_ordered (&sleep_list, &cur->elem, cmp_wakeup, NULL);
  update_next_tick (wake_tick);
  thread_block ();
  intr_set_level (old);
}

/* Priority API */
void
thread_set_priority (int new_priority)
{
  enum intr_level old_level = intr_disable ();
  struct thread *cur = thread_current ();
  cur->original_priority = new_priority;
  thread_update_priority (cur);

  /*ready list의 가장 높은 priority 스레드와 비교 */
  if (!list_empty (&ready_list))
  {
    struct thread *top = list_entry (list_front (&ready_list),
                                     struct thread, elem);
    if (top->priority > cur->priority)
      thread_yield ();
  }

  intr_set_level (old_level);
}

int thread_get_priority (void) { return thread_current ()->priority; }

/* Donation helpers */
void thread_update_priority (struct thread *t) {
  int maxp = t->original_priority;
  if (!list_empty (&t->donations)) {
    struct thread *donor = list_entry (list_front (&t->donations), struct thread, donation_elem);
    if (donor->priority > maxp) maxp = donor->priority;
  }
  t->priority = maxp;
}

void thread_donate_priority (void) {
  struct thread *cur = thread_current ();
  for (int depth = 0; depth < 8; depth++) {
    struct lock *lock = cur->wait_on_lock;
    if (!lock || !lock->holder) break;
    struct thread *holder = lock->holder;
    if (holder->priority < cur->priority)
      holder->priority = cur->priority;
    cur = holder;
  }
}

void thread_remove_donation (struct lock *lock) {
  struct thread *cur = thread_current ();
  struct list_elem *e = list_begin (&cur->donations);
  while (e != list_end (&cur->donations)) {
    struct thread *t = list_entry (e, struct thread, donation_elem);
    if (t->wait_on_lock == lock)
      e = list_remove (&t->donation_elem);
    else
      e = list_next (e);
  }
  thread_update_priority (cur);
}

/* Scheduling core */
static struct thread *next_thread_to_run (void) {
  if (list_empty (&ready_list)) return idle_thread;
  return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

static void schedule (void) {
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next) prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

void thread_schedule_tail (struct thread *prev) {
  struct thread *cur = running_thread ();
#ifdef USERPROG
  process_activate ();
#endif
  if (prev && prev->status == THREAD_DYING) palloc_free_page (prev);
}

static void idle (void *aux UNUSED) {
  for(;;) {
    enum intr_level old = intr_disable ();
    if (list_empty (&ready_list))
      asm volatile ("sti; hlt" : : : "memory");
    intr_set_level (old);
    thread_yield ();
  }
}

static void kernel_thread (thread_func *function, void *aux) {
  intr_enable ();
  function (aux);
  thread_exit ();
}

/* Misc */
const char *thread_name (void) { return thread_current ()->name; }
struct thread *thread_current (void) { return running_thread (); }
tid_t thread_tid (void) { return thread_current ()->tid; }

void thread_exit (void) {
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

void thread_yield (void) {
  struct thread *cur = thread_current ();
  enum intr_level old = intr_disable ();
  if (cur != idle_thread)
    list_insert_ordered (&ready_list, &cur->elem, cmp_ready, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old);
}

void thread_foreach (thread_action_func *action, void *aux) {
  for (struct list_elem *e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
    action (list_entry (e, struct thread, allelem), aux);
}

static struct thread *running_thread (void) {
  uint32_t *esp; asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) ((uintptr_t) esp & ~ (PGSIZE - 1));
}

static void init_thread (struct thread *t, const char *name, int priority) {
  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->original_priority = priority;
  list_init (&t->donations);
  t->wait_on_lock = NULL;
  t->wakeup_tick = 0;
  t->age = 0;
  t->mlfqs_queue_level = Q2;
  t->mlfqs_ticks = 0;
  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

static tid_t allocate_tid (void) {
  static tid_t next_tid = 1;
  tid_t tid; lock_acquire (&tid_lock); tid = next_tid++; lock_release (&tid_lock); return tid;
}
