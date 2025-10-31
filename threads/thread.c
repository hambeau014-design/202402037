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
static struct list *mlfqs_get_queue(enum mlfqs_queue level);
bool priority_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
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
   scheduled before thread_create()
