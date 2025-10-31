/* thread.c */

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

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
// ===================================================================
// *** MODIFICATION: ready_list를 MLFQS용 큐로 확장 ***
static struct list ready_list;      // 일반 우선순위 스케줄링용 (기존)
static struct list ready_list_q0;   // MLFQS Q0
static struct list ready_list_q1;   // MLFQS Q1
static struct list ready_list_q2;   // MLFQS Q2
// ===================================================================

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* List of process in sleep */
static struct list sleep_list;
static int64_t next_tick_to_wakeup = INT64_MAX;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
{
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent in idle state. */
static long long kernel_ticks;  /* # of timer ticks spent in kernel threads. */
static long long user_ticks;    /* # of timer ticks spent in user processes. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

// ===================================================================
// *** MODIFICATION: 헬퍼 함수 구현 ***

/* 주어진 큐 레벨에 해당하는 list 구조체를 반환합니다. */
static struct list *mlfqs_get_queue(enum mlfqs_queue level) {
  switch (level) {
    case Q0: return &ready_list_q0;
    case Q1: return &ready_list_q1;
    case Q2: return &ready_list_q2;
    default: NOT_REACHED ();
  }
}

/* 우선순위 비교 함수: t1의 우선순위가 t2보다 높으면(list에서 더 앞쪽에 위치해야 하면) true 반환 */
bool
priority_less (const struct list_elem *a, const struct list_elem *b,
               void *aux UNUSED)
{
  struct thread *t1 = list_entry (a, struct thread, elem);
  struct thread *t2 = list_entry (b, struct thread, elem);
  return t1->priority > t2->priority; 
}
// ===================================================================


/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   all situations, but it does in ours because a thread doesn't
   have any special prepocessing or need a separate stack.

   We also initialize the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is safe to call this function multiple times. */
void
thread_init (void)
{
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);
    
    // ===================================================================
    // *** MODIFICATION: MLFQS 큐 초기화 ***
    if (thread_mlfqs) {
        list_init (&ready_list_q0);
        list_init (&ready_list_q1);
        list_init (&ready_list_q2);
    } else {
        list_init (&ready_list); // 기존 ready_list 초기화
    }
    // ===================================================================

    list_init (&all_list);
    list_init (&sleep_list);

    /* Set up a thread structure for the running code. */
    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}

/* Starts the scheduler -- enables interrupts and runs the idle
   thread.  After calling this function, no one should ever call
   thread_yeild() directly, since the scheduler should be managing
   the running threads. */
void
thread_start (void)
{
    /* Create the idle thread. */
    idle_thread = thread_create ("idle", PRI_MIN, idle, NULL);

    /* Start preemptive thread scheduling. */
    intr_enable ();
}

/* Called by the timer interrupt handler at each timer tick.
   Each time a timer tick comes in, we increment `thread_ticks'.
   The first TIME_SLICE* threads to be run are chosen in round-robin
   fashion.  After that, the scheduler makes a new choice. */
void
thread_tick (void)
{
    struct thread *cur = thread_current ();

    /* Update stats. */
    if (cur == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (cur->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    // --- 스케줄러 로직: Aging 및 Time Slice 처리 ---
    if (!thread_mlfqs) 
    { 
        /* 1. 에이징 로직: ready_list의 스레드 age 증가 및 우선순위 조정 */
        enum intr_level old_level = intr_disable();
        struct list_elem *e = list_begin(&ready_list);
        
        while (e != list_end(&ready_list))
        {
            struct thread *t = list_entry (e, struct thread, elem);
            t->age++;
            
            if (t->age >= AGING_TICKS)
            {
                t->age = 0;
                
                // 우선순위 한 단계 상승 (최대 PRI_DEFAULT까지)
                if (t->priority < PRI_DEFAULT) {
                    t->priority++;
                    
                    // 우선순위가 바뀌었으므로 리스트에서 제거 후 재삽입 (정렬 유지)
                    e = list_remove(&t->elem);
                    list_insert_ordered (&ready_list, &t->elem, priority_less, NULL);
                    
                    // 우선순위가 높아졌다면 즉시 선점 고려
                    if (t->priority > cur->priority) {
                        intr_yield_on_return ();
                    }
                    
                    continue; // 재삽입 후 다음 요소를 위해 continue
                }
            }
            e = list_next(e);
        }
        intr_set_level(old_level);
        
        /* 2. 일반 우선순위 스케줄링의 시간 슬라이스는 선점형 로직으로 대체되었음 */
        // 기존의 라운드 로빈 로직 제거 (TIME_SLICE 기반)
    }
    else /* MLFQS 모드 */
    {
        enum intr_level old_level = intr_disable();
        
        // --- 1. Aging/Promotion (대기 중인 모든 스레드) ---
        // Q2 -> Q1 -> Q0 순서로 순회하며 승급
        bool promoted_to_q0 = false; 

        for (enum mlfqs_queue q = Q2; q >= Q0; q--) 
        {
            struct list *q_list = mlfqs_get_queue(q);
            struct list_elem *e = list_begin(q_list);
            
            while (e != list_end(q_list))
            {
                struct thread *t = list_entry (e, struct thread, elem);
                t->age++;

                if (t->age >= AGING_TICKS)
                {
                    t->age = 0;
                    if (t->mlfqs_queue_level > Q0) 
                    {
                        t->mlfqs_queue_level--; // 승급
                        
                        e = list_remove(&t->elem);
                        list_push_back (mlfqs_get_queue(t->mlfqs_queue_level), &t->elem);
                        
                        if (t->mlfqs_queue_level == Q0)
                            promoted_to_q0 = true;
                        
                        continue; // 삭제되었으므로 continue
                    }
                }
                e = list_next(e);
            }
        }
        
        // Promotion에 의한 선점: Q0으로 승급되어 현재 실행 중인 스레드가 Q0이 아니라면 선점
        if (promoted_to_q0 && cur->mlfqs_queue_level != Q0) {
            intr_yield_on_return ();
        }

        // --- 2. Demotion (현재 실행 중인 스레드) ---
        if (cur != idle_thread) {
            cur->mlfqs_ticks++;
            
            int current_slice = (cur->mlfqs_queue_level == Q0) ? MLFQS_Q0_SLICE : 
                                (cur->mlfqs_queue_level == Q1) ? MLFQS_Q1_SLICE : 
                                                                 MLFQS_Q2_SLICE;
                                                                 
            if (cur->mlfqs_ticks >= current_slice) 
            {
                cur->mlfqs_ticks = 0;
                
                if (cur->mlfqs_queue_level < Q2) // Q2가 아닐 때만 강등 가능
                {
                    cur->mlfqs_queue_level++; // 강등
                }
                
                // 시간 슬라이스 소모 완료 또는 강등 후 yield (스케줄러 호출)
                thread_yield (); 
            }
        }
        intr_set_level(old_level);
    }
    // ===================================================================

    /* Enforce preemption. (기존 라운드 로빈 로직) */
    if (++thread_ticks >= TIME_SLICE && !thread_mlfqs)
        intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given PRIORITY,
   which executes FUNCTION, passing AUX as the argument, and adds it
   to the ready queue.  Returns the thread identifier for the new
   thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Changes to *aux passed by the 
   caller may be lost if the thread exits before the return. */
tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux)
{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;

    ASSERT (function != NULL);

    /* Allocate thread. */
    t = palloc_get_page (PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* Initialize thread. */
    init_thread (t, name, priority);
    tid = t->tid = allocate_tid ();

    /* Stack frame for kernel_thread(). */
    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    /* Stack frame for switch_entry(). */
    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void)) kernel_thread;

    /* Stack frame for switch_threads(). */
    sf = alloc_frame (t, sizeof *sf);
    sf->eip = (void (*) (void)) switch_entry;
    sf->edi = (uint32_t) t;
    sf->esi = (uint32_t) t;
    sf->ebp = (uint32_t) t;

    /* Add to run queue. */
    thread_unblock (t);

    return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void)
{
    ASSERT (!intr_context ());
    ASSERT (intr_get_level () == INTR_OFF);
    thread_current ()->status = THREAD_BLOCKED;
    schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an all-purpose wake-up function.

   If thread_start() has been called, T is moved to the ready list.
   If T's priority is higher than the current running thread, 
   the current thread must be preempted. */
void
thread_unblock (struct thread *t)
{
    enum intr_level old_level;

    ASSERT (is_thread (t));
    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED);

    // ===================================================================
    // *** MODIFICATION: 큐 삽입 및 선점 로직 ***
    if (thread_mlfqs) {
        // MLFQS: 해당 큐의 맨 뒤에 삽입 (FIFO within queue)
        list_push_back (mlfqs_get_queue(t->mlfqs_queue_level), &t->elem);
        t->age = 0; // 큐에 진입 시 age 초기화
        
        // MLFQS Q0 즉시 선점: Q0에 새 스레드가 추가되었고 현재 스레드가 Q0이 아니면 선점
        if (t->mlfqs_queue_level == Q0 && thread_current()->mlfqs_queue_level != Q0) {
            intr_yield_on_return ();
        }

    } else {
        // 우선순위 스케줄링: 우선순위 순으로 ready_list에 삽입
        list_insert_ordered (&ready_list, &t->elem, priority_less, NULL);
        t->age = 0; // 큐에 진입 시 age 초기화
        
        // 선점 체크: Unblock된 스레드의 우선순위가 현재 스레드보다 높으면 즉시 양보(yield)
        if (t->priority > thread_current()->priority)
            thread_yield ();
    }
    // ===================================================================
    
    t->status = THREAD_READY;
    intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
    return thread_current ()->name;
}

/* Returns the running thread.
   This is equivalent to thread_current (), but prettier. */
struct thread *
running_thread (void)
{
    uint32_t *esp;

    asm ("mov %%esp, %0" : "=g" (esp));
    return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
    return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named NAME. */
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

    // ===================================================================
    // *** MODIFICATION: 에이징/MLFQS 멤버 초기화 ***
    t->age = 0; 
    if (thread_mlfqs) {
      t->mlfqs_queue_level = Q0; // MLFQS는 Q0에서 시작
      t->mlfqs_ticks = 0;
    }
    // ===================================================================

    t->magic = THREAD_MAGIC;
    list_push_back (&all_list, &t->allelem);
}

/* Chooses and returns the next thread to be scheduled.  If no
   threads are ready to run, returns the idle thread.

   You can rely on this function to have a novel implementation in
   every project. */
static struct thread *
next_thread_to_run (void)
{
    // ===================================================================
    // *** MODIFICATION: MLFQS 또는 우선순위 스케줄링에 따라 선택 ***
    if (thread_mlfqs) {
        // Q0 > Q1 > Q2 순서로 비어 있지 않은 가장 높은 큐의 스레드를 선택 (FIFO within queue)
        if (!list_empty(&ready_list_q0))
            return list_entry (list_pop_front (&ready_list_q0), struct thread, elem);
        if (!list_empty(&ready_list_q1))
            return list_entry (list_pop_front (&ready_list_q1), struct thread, elem);
        if (!list_empty(&ready_list_q2))
            return list_entry (list_pop_front (&ready_list_q2), struct thread, elem);
        
        return idle_thread;
    } else {
        // 우선순위 스케줄링: ready_list에서 가장 앞쪽(최고 우선순위) 스레드를 꺼낸다.
        if (!list_empty (&ready_list))
            return list_entry (list_pop_front (&ready_list), struct thread, elem);
        else
            return idle_thread;
    }
    // ===================================================================
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous process was dying, destroying it.

   The thread passed to this function cannot be the running thread. */
void
thread_schedule_tail (struct thread *prev)
{
    struct thread *cur = running_thread ();
    
    ASSERT (intr_get_level () == INTR_OFF);
    /* Mark us as running. */
    cur->status = THREAD_RUNNING;

    /* Start new time slice. */
    thread_ticks = 0;

#ifdef USERPROG
    /* Activate the new thread's page tables. */
    process_activate ();
#endif

    /* If the thread switched from is dying, destroy its struct
       thread.  This must happen late so that thread_exit() doesn't
       pull out the rug under itself.  (We don't free
       initial_thread because its memory was not obtained via
       palloc().) */
    if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
        {
            ASSERT (prev != cur);
            palloc_free_page (prev);
        }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
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

/* Returns a tid to use for a new thread. */
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
/* Finish up during a switch. */

/* ... (나머지 기존 함수: thread_get_tid, thread_current, thread_exit) ... */


/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;
    
    old_level = intr_disable ();
    if (cur != idle_thread) {
        // ===================================================================
        // *** MODIFICATION: 큐 삽입 로직 ***
        if (thread_mlfqs) {
            // MLFQS: 현재 큐(강등된 큐일 수 있음)의 맨 뒤에 삽입
            list_push_back (mlfqs_get_queue(cur->mlfqs_queue_level), &cur->elem);
            cur->age = 0; // Yield 시 age 초기화
        } else {
            // 우선순위 스케줄링: 우선순위 순으로 ready_list에 삽입
            list_insert_ordered (&ready_list, &cur->elem, priority_less, NULL);
            cur->age = 0; // Yield 시 age 초기화
        }
        // ===================================================================
    }
    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}


/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority)
{
    enum intr_level old_level;
    old_level = intr_disable ();
    
    // ===================================================================
    // *** MODIFICATION: 우선순위 변경 및 선점 로직 ***
    // MLFQS 모드에서는 우선순위 설정이 스케줄러에 영향을 미치지 않도록 처리
    if (!thread_mlfqs) {
        thread_current ()->priority = new_priority;

        // 우선순위가 낮아졌고, ready_list에 더 높은 우선순위 스레드가 있으면 즉시 선점
        if (!list_empty (&ready_list)) {
            struct thread *highest_ready = list_entry (list_front (&ready_list), struct thread, elem);
            if (thread_current ()->priority < highest_ready->priority)
                thread_yield (); 
        }
    }
    // ===================================================================
    
    intr_set_level (old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* ... (thread_get_nice, thread_set_nice, thread_get_recent_cpu, thread_get_load_avg, thread_foreach) ... */
