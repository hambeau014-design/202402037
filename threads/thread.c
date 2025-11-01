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
static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define AGING_INTERVAL 1
#define AGING_LIMIT 20

static unsigned thread_ticks;
static int64_t ready_counter = 0;

static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

static bool cmp_ready_fifo (const struct list_elem *a,
                            const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  if (ta->priority != tb->priority)
    return ta->priority > tb->priority;
  return ta->ready_stamp < tb->ready_stamp;
}

/* Initializes the threading system. */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling. */
void
thread_start (void)
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);
  sema_down (&idle_started);
}

/* Called by timer interrupt handler every tick. */
void
thread_tick (void)
{
  struct thread *t = thread_current ();

  if (t == idle_thread)
    idle_ticks++;
  else if (t->pagedir != NULL)
    user_ticks++;
  else
    kernel_ticks++;

  /* Aging every tick for ready_list */
  if (!thread_mlfqs)
    {
      struct list_elem *e = list_begin (&ready_list);
      bool boosted = false;

      while (e != list_end (&ready_list))
        {
          struct thread *thr = list_entry (e, struct thread, elem);
          e = list_next (e);

          thr->age++;
          if (thr->age >= AGING_LIMIT && thr->priority < PRI_MAX)
            {
              thr->age = 0;
              thr->priority++;
              list_remove (&thr->elem);
              thr->ready_stamp = ++ready_counter;
              list_insert_ordered (&ready_list, &thr->elem, cmp_ready_fifo, NULL);
              boosted = true;
            }
        }

      if (boosted && !list_empty (&ready_list))
        {
          struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
          if (top->priority > thread_current ()->priority)
            intr_yield_on_return ();
        }
    }

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Returns the running thread. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
  return t;
}

/* Creates a new thread. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
  struct thread *t;
  tid_t tid;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  struct kernel_thread_frame *kf = &t->tf.kernel_tf;
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  t->tf.eip = (void (*)(void)) kernel_thread;
  t->tf.eflags = FLAG_IF;
  t->tf.esp = PHYS_BASE;

  thread_unblock (t);

  if (t->priority > thread_current ()->priority)
    thread_yield ();

  return tid;
}

/* Unblocks a thread. */
void
thread_unblock (struct thread *t)
{
  enum intr_level old = intr_disable ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_BLOCKED);

  t->status = THREAD_READY;
  t->age = 0;
  t->ready_stamp = ++ready_counter;
  list_insert_ordered (&ready_list, &t->elem, cmp_ready_fifo, NULL);

  if (t != thread_current () && t->priority > thread_current ()->priority)
    intr_yield_on_return ();

  intr_set_level (old);
}

/* Blocks the current thread. */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);
  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Yields the CPU. */
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

/* Schedules a new thread. */
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

/* Chooses the next thread to run. */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Idle thread function. */
static void
idle (void *aux UNUSED)
{
  struct semaphore *idle_started = aux;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;)
    {
      intr_disable ();
      thread_block ();
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Initializes a thread. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (priority >= PRI_MIN && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->tf.eip = NULL;
  t->priority = priority;
  t->age = 0;
  t->ready_stamp = ++ready_counter;
  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

/* Returns a running thread struct. */
static struct thread *
running_thread (void)
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Allocates TID safely. */
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
