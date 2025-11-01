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

/* Aging: ready 상태에서 age가 20이 되면 priority +1, age=0 */
#define AGING_LIMIT  20

struct kernel_thread_frame {
    void *eip;
    thread_func *function;
    void *aux;
};

/* Non-MLFQS ready queue (priority desc, FIFO among equals). */
static struct list ready_list;
/* All threads list. */
static struct list all_list;

/* Sleepers (sorted by wakeup_tick asc). */
static struct list sleep_list;
static int64_t next_wakeup = INT64_MAX;

/* Idle thread & initial thread. */
static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;
static struct kernel_thread_frame;

/* Statistics. */
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

/* RR tick counter. */
static unsigned thread_ticks;

/* FIFO tie-breaker for ready queue. */
static int64_t ready_counter = 0;

/* MLFQS flag (threads 단계에서는 기본 false). */
bool thread_mlfqs = false;

/* Forward decls. */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* ---------------- Comparators ---------------- */

/* ready_list: priority 내림차순, 같으면 ready_stamp 오름차순(FIFO) */
static bool
cmp_ready_fifo (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  if (ta->priority != tb->priority) return ta->priority > tb->priority;
  return ta->ready_stamp < tb->ready_stamp;
}

/* sleep_list: wakeup_tick 오름차순 */
static bool
cmp_wakeup (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->wakeup_tick < tb->wakeup_tick;
}

/* ---------------- Sleep helpers ---------------- */

static void
sleepers_try_wake (int64_t now_ticks)
{
  while (!list_empty (&sleep_list))
    {
      struct thread *t = list_entry (list_front (&sleep_list), struct thread, elem);
      if (t->wakeup_tick > now_ticks) break;
      list_pop_front (&sleep_list);
      thread_unblock (t);
    }
  next_wakeup = list_empty (&sleep_list)
                  ? INT64_MAX
                  : list_entry (list_front (&sleep_list), struct thread, elem)->wakeup_tick;
}

/* ---------------- Init ---------------- */

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
  /* Create the idle thread. */
  idle_thread = thread_create ("idle", PRI_MIN, idle, NULL);
  intr_enable ();
}

/* ---------------- Tick ---------------- */

void
thread_tick (void)
{
  struct thread *cur = thread_current ();
  int64_t now = timer_ticks ();

  /* wake up sleeping threads */
  if (next_wakeup <= now)
    sleepers_try_wake (now);

  /* stats (threads 단계에선 user_ticks는 보통 0으로 유지) */
  if (cur == idle_thread) idle_ticks++;
  else kernel_ticks++;

  /* Aging: ready_list에서 대기 중인 스레드 age 증가 & 필요 시 우선순위 상승 */
  if (!thread_mlfqs)
    {
      bool boosted = false;
      for (struct list_elem *e = list_begin (&ready_list);
           e != list_end (&ready_list); )
        {
          struct thread *t = list_entry (e, struct thread, elem);
          struct list_elem *next = list_next (e);

          t->age++;
          if (t->age >= AGING_LIMIT && t->priority < PRI_MAX)
            {
              t->age = 0;
              t->priority++;
              list_remove (&t->elem);
              t->ready_stamp = ++ready_counter;
              list_insert_ordered (&ready_list, &t->elem, cmp_ready_fifo, NULL);
              boosted = true;
            }
          e = next;
        }
      if (boosted && !list_empty (&ready_list))
        {
          struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
          if (top->priority > cur->priority)
            intr_yield_on_return ();
        }
    }

  /* RR: TIME_SLICE마다 선점 */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* ---------------- API ---------------- */

const char *
thread_name (void)
{
  return thread_current ()->name;
}

struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();
  ASSERT (t->status == THREAD_RUNNING);
  return t;
}

tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

void
thread_exit (void)
{
  ASSERT (!intr_context ());

  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old = intr_disable ();
  if (cur != idle_thread)
    {
      cur->status = THREAD_READY;
      cur->age = 0;
      cur->ready_stamp = ++ready_counter;
      list_insert_ordered (&ready_list, &cur->elem, cmp_ready_fifo, NULL);
    }
  schedule ();
  intr_set_level (old);
}

void
thread_sleep_until (int64_t wake_tick)
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

/* ---------------- Create / Block / Unblock ---------------- */

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

  /* Set up stack frames. (Pintos 표준) */
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

  /* 더 높은 우선순위면 양보 */
  if (t->priority > thread_current ()->priority)
    thread_yield ();

  return tid;
}

void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);
  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

static void
enqueue_ready (struct thread *t)
{
  t->age = 0;
  t->ready_stamp = ++ready_counter;
  list_insert_ordered (&ready_list, &t->elem, cmp_ready_fifo, NULL);
}

void
thread_unblock (struct thread *t)
{
  enum intr_level old = intr_disable ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_BLOCKED);

  enqueue_ready (t);
  t->status = THREAD_READY;

  /* 즉시 선점 */
  if (t != thread_current () && t->priority > thread_current ()->priority)
    intr_yield_on_return ();

  intr_set_level (old);
}

/* ---------------- Priority ---------------- */

void
thread_set_priority (int new_priority)
{
  enum intr_level old = intr_disable ();
  struct thread *cur = thread_current ();
  cur->original_priority = new_priority;
  thread_update_priority (cur);

  if (!list_empty (&ready_list))
    {
      struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
      if (top->priority > cur->priority)
        thread_yield ();
    }
  intr_set_level (old);
}

int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* 우선순위 도네이션 관련 */
void
thread_update_priority (struct thread *t)
{
  int maxp = t->original_priority;
  if (!list_empty (&t->donations))
    {
      struct thread *donor = list_entry (list_front (&t->donations), struct thread, donation_elem);
      if (donor->priority > maxp) maxp = donor->priority;
    }
  t->priority = maxp;
}

void
thread_donate_priority (void)
{
  struct thread *cur = thread_current ();
  for (int depth = 0; depth < 8; depth++)
    {
      struct lock *lk = cur->wait_on_lock;
      if (!lk || !lk->holder) break;
      struct thread *holder = lk->holder;
      if (holder->priority < cur->priority)
        holder->priority = cur->priority;
      cur = holder;
    }
}

void
thread_remove_donation (struct lock *lock)
{
  struct thread *cur = thread_current ();
  for (struct list_elem *e = list_begin (&cur->donations);
       e != list_end (&cur->donations); )
    {
      struct thread *t = list_entry (e, struct thread, donation_elem);
      if (t->wait_on_lock == lock)
        e = list_remove (&t->donation_elem);
      else
        e = list_next (e);
    }
  thread_update_priority (cur);
}

/* ---------------- Scheduler core ---------------- */

static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

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

void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  if (prev != NULL && prev->status == THREAD_DYING)
    palloc_free_page (prev);
  (void)cur;
}

/* ---------------- Init helpers ---------------- */

static void
init_thread (struct thread *t, const char *name, int priority)
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
  t->qlevel = Q0;
  t->run_ticks_in_level = 0;

  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

static struct thread *
running_thread (void)
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) ((uintptr_t) esp & ~(PGSIZE - 1));
}

static tid_t
allocate_tid (void)
{
  static tid_t next_tid = 1;
  tid_t tid;
  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);
  return tid;
}

/* ---------------- Kernel thread trampoline ---------------- */

static void
kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);
  intr_enable ();       /* Enable interrupts. */
  function (aux);       /* Run the thread function. */
  thread_exit ();       /* If function returns, kill the thread. */
}

/* ---------------- Idle ---------------- */

static void
idle (void *aux UNUSED)
{
  for (;;)
    {
      enum intr_level old = intr_disable ();
      if (list_empty (&ready_list))
        asm volatile ("sti; hlt" : : : "memory");
      intr_set_level (old);
      thread_yield ();
    }
}
