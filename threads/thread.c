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
#include "devices/timer.h" // thread_sleep_until에서 timer_ticks()를 사용하기 위해 추가

#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Magic number for detection of stack overflow. */
#define THREAD_MAGIC 0xcd6abf4b

/* The scheduler's time slice for round-robin. */
#define TIME_SLICE 4

/* List of all ready threads, ordered by priority (and ready_stamp for FIFO). */
static struct list ready_list;

/* List of all threads ever created. */
static struct list all_list;

/* Idle thread, initialized by thread_start(). */
static struct thread *idle_thread;

/* Initial thread, the thread running when the kernel is started. */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Statistics. */
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

/* Aging related constants. */
#define AGING_INTERVAL 1
#define AGING_LIMIT 20

/* Next time slice to be given to a thread. */
static unsigned thread_ticks;

/* Ready counter for FIFO ordering of equal-priority threads. */
static int64_t ready_counter = 0;

/* Function prototypes. */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static bool is_thread (struct thread *) UNUSED;
static void init_thread (struct thread *, const char *name, int priority);
static struct thread *next_thread_to_run (void);
static void schedule (void);
static tid_t allocate_tid (void);

/* Comparison function for ready_list: higher priority first, FIFO for ties. */
static bool cmp_ready_fifo (const struct list_elem *a,
                            const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  if (ta->priority != tb->priority)
    return ta->priority > tb->priority;
  return ta->ready_stamp < tb->ready_stamp;
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
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

/* Starts preemptive thread scheduling by enabling interrupts. */
void
thread_start (void)
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  idle_thread = thread_create ("idle", PRI_MIN, idle, &idle_started);
  sema_down (&idle_started);

  intr_enable ();
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

/* Prints thread statistics. */
void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
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

/* Returns the current thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
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

  /* Set up stack frame to run kernel_thread(function, aux). */

  // 1. 스택 포인터를 스레드의 끝 주소로 설정
  t->stack = (uint8_t *) t + PGSIZE;
  
  // 2. intr_frame의 레지스터들을 0으로 초기화
  struct intr_frame *if_ = (struct intr_frame *) t->stack - 1;
  memset (if_, 0, sizeof *if_);
  
  // 3. EFLAGS 설정: 인터럽트 활성화
  if_->eflags = FLAG_IF;

  // 4. EIP 설정: thread_create가 반환되면 intr_exit()에서 EIP를 가져와 kernel_thread를 실행
  if_->eip = (void (*) (void)) kernel_thread;

  // 5. switch_entry_frame 설정
  struct switch_entry_frame *ef = (struct switch_entry_frame *) if_ - 1;
  ef->eip = (void (*) (void)) kernel_thread;
  t->stack = (uint8_t *) ef;

  // 6. switch_threads_frame 설정
  struct switch_threads_frame *sf = (struct switch_threads_frame *) t->stack - 1;
  sf->eip = (void (*) (void)) intr_exit; // kernel_thread 실행 후 intr_exit으로 돌아가도록
  t->stack = (uint8_t *) sf;


  // 인자 설정 (kernel_thread의 인자: function, aux)
  // kernel_thread 함수 내부에서 스택을 정리하기 위한 DUMMY RET(함수 종료 후 돌아갈 주소) 설정
  * (uint32_t *) t->stack -= sizeof (uint32_t); 
  * (uint32_t *) t->stack = (uint32_t) 0;

  // aux와 function을 인수로 푸시 (스택은 역순으로 채워짐)
  t->stack -= sizeof (uint32_t);
  * (uint32_t *) t->stack = (uint32_t) aux;
  t->stack -= sizeof (uint32_t);
  * (uint32_t *) t->stack = (uint32_t) function;

  // 스택 포인터 최종 설정
  t->stack = (uint8_t *)sf;


  thread_unblock (t);

  /* Preemption check. */
  if (t->priority > thread_current ()->priority)
    thread_yield ();

  return tid;
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

  /* Preemption check. */
  if (t != thread_current () && t->priority > thread_current ()->priority)
    intr_yield_on_return ();

  intr_set_level (old);
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

/* Thread exit. */
void
thread_exit (void)
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

/* Executes FUNC for each thread in the system. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority. */
void
thread_set_priority (int new_priority)
{
  thread_current ()->priority = new_priority;
  thread_current ()->original_priority = new_priority; // Base priority

  // Donation check
  thread_donate_priority ();

  // Preemption check
  if (!list_empty (&ready_list))
    {
      struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
      if (top->priority > thread_current ()->priority)
        thread_yield ();
    }
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* Sleeps current thread until WAKE_TICK. (Sleep implementation) */
void
thread_sleep_until (int64_t wake_tick)
{
  struct thread *t = thread_current ();
  enum intr_level old = intr_disable ();

  ASSERT (t != idle_thread);

  t->wakeup_tick = wake_tick;
  thread_block ();
  
  intr_set_level (old);
}

/* Donation helpers */
void thread_update_priority (struct thread *t) { /* TODO */ }
void thread_donate_priority (void) { /* TODO */ }
void thread_remove_donation (struct lock *lock) { /* TODO */ }

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
  t->priority = priority;
  t->original_priority = priority;
  
  // Donation fields init
  list_init (&t->donations);
  t->wait_on_lock = NULL;

  t->age = 0;
  t->ready_stamp = ++ready_counter;
  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

/* Returns the running thread. */
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

/* Chooses the next thread to run. */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
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

/* Completion of a thread switch. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (is_thread (cur));
  ASSERT (intr_get_level () == INTR_OFF);

  cur->status = THREAD_RUNNING;

  if (prev != NULL && prev->status == THREAD_DYING)
    {
      ASSERT (prev != initial_thread);
      palloc_free_page (prev);
    }
}
