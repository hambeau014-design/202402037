/* thread.c - 완성 버전 */

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

static struct list ready_list;
static struct list all_list;
static struct list sleep_list;

static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

static unsigned thread_ticks;
static long long idle_ticks, kernel_ticks, user_ticks;
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* ----------------------- 비교 함수 ----------------------- */

bool
compare_thread_priority_elem (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority;
}

bool
compare_thread_priority_donation (const struct list_elem *a,
                                  const struct list_elem *b,
                                  void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, donation_elem);
  const struct thread *tb = list_entry (b, struct thread, donation_elem);
  return ta->priority > tb->priority;
}

/* ----------------------- thread core ----------------------- */

void
thread_init (void)
{
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

void
thread_start (void)
{
  idle_thread = thread_create ("idle", PRI_MIN, idle, NULL);
  intr_enable ();
}

void
thread_tick (void)
{
  struct thread *cur = thread_current ();
  if (cur == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (cur->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux)
{
  struct thread *t;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  struct kernel_thread_frame *kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  struct switch_entry_frame *ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  struct switch_threads_frame *sf = alloc_frame (t, sizeof *sf);
  sf->eip = (void (*) (void)) switch_entry;
  sf->ebp = 0;

  old_level = intr_disable ();
  thread_unblock (t);
  intr_set_level (old_level);

  if (t->priority > thread_current ()->priority)
    thread_yield ();

  return tid;
}

/* ----------------------- block/unblock ----------------------- */

void
thread_block (void)
{
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

  list_insert_ordered (&ready_list, &t->elem, compare_thread_priority_elem, NULL);
  t->status = THREAD_READY;

  /* 선점 처리 */
  if (t->priority > thread_current ()->priority)
  {
    if (!intr_context ())
      thread_yield ();
    else
      intr_yield_on_return ();
  }

  intr_set_level (old_level);
}

/* ----------------------- 우선순위 관련 ----------------------- */

void
thread_set_priority (int new_priority)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level = intr_disable ();

  cur->original_priority = new_priority;
  thread_update_priority (cur);

  /* ready list에서 더 높은 우선순위 있으면 양보 */
  if (!list_empty (&ready_list)) {
    struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
    if (top->priority > cur->priority)
      thread_yield ();
  }

  intr_set_level (old_level);
}

int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

void
thread_update_priority (struct thread *t)
{
  int max_priority = t->original_priority;
  if (!list_empty (&t->donations)) {
    struct thread *donor = list_entry (list_front (&t->donations), struct thread, donation_elem);
    if (donor->priority > max_priority)
      max_priority = donor->priority;
  }
  t->priority = max_priority;
}

/* ----------------------- donation 관련 ----------------------- */

void
thread_donate_priority (void)
{
  struct thread *cur = thread_current ();
  for (int depth = 0; depth < 8; depth++) {
    struct lock *lock = cur->wait_on_lock;
    if (!lock || !lock->holder)
      break;
    struct thread *holder = lock->holder;
    if (holder->priority < cur->priority)
      holder->priority = cur->priority;
    cur = holder;
  }
}

void
thread_remove_donation (struct lock *lock)
{
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

/* ----------------------- scheduling core ----------------------- */

static void
schedule (void)
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

static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* 기타 기본 함수(init_thread 등)는 동일) */
