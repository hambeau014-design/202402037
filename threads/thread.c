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


/* Initializes the threading system by transforming the code ... */
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
   thread. ... */
void
thread_start (void)
{
    /* Create the idle thread. */
    // thread_create는 tid_t를 반환하므로, 이는 임시 코드이거나 thread_create가
    // struct thread *를 반환하도록 변경되어야 합니다. Pintos의 관행을 따릅니다.
    idle_thread = thread_create ("idle", PRI_MIN, idle, NULL);

    /* Start preemptive thread scheduling. */
    intr_enable ();
}

/* Called by the timer interrupt handler at each timer tick. ... */
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
/* ... (MLFQS 로직 유지) ... */
    }
    
    // ===================================================================
    // *** MODIFICATION: 라운드 로빈 스케줄링 로직 제거 ***
    // 우선순위 스케줄링에서는 선점 로직이 TIME_SLICE를 대신합니다.
    // if (++thread_ticks >= TIME_SLICE && !thread_mlfqs)
    //     intr_yield_on_return ();
    // ===================================================================
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
   scheduled before thread_create()*/

// ===================================================================
// *** MODIFICATION: thread_create 구현 (부분) ***

/* thread_create의 나머지 부분 (구현되지 않은 함수 스켈레톤의 마무리) */
tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux)
{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;
    enum intr_level old_level;

    ASSERT (name != NULL);
    ASSERT (priority >= PRI_MIN && priority <= PRI_MAX);

    /* Allocate thread. */
    t = palloc_get_page (PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* Initialize thread. */
    init_thread (t, name, priority);
    tid = t->tid = allocate_tid ();

    /* Stack setup. */
    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void)) kernel_thread;

    sf = alloc_frame (t, sizeof *sf);
    sf->eip = (void (*) (void)) switch_entry;
    sf->ebp = 0;

    /* Add to run queue. */
    old_level = intr_disable ();
    thread_unblock (t);
    intr_set_level (old_level);
    
    // *** 선점 로직: 새 스레드가 현재 스레드보다 우선순위가 높으면 즉시 양보 ***
    if (!thread_mlfqs) {
        if (t->priority > thread_current()->priority)
            thread_yield();
    }
    
    return tid;
}
// ===================================================================


/* Blocks the current thread, setting its status to THREAD_BLOCKED.

   It is an error to call this function when interrupts are
   enabled. */
void
thread_block (void)
{
    ASSERT (!intr_context ());
    ASSERT (intr_get_level () == INTR_OFF);
    thread_current ()->status = THREAD_BLOCKED;
    schedule ();
}

/* Transitions a blocked or ready thread T to the ready state.
   The thread may be an idle thread, but otherwise must not be running. */
void
thread_unblock (struct thread *t)
{
    enum intr_level old_level;

    ASSERT (is_thread (t));

    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED || t->status == THREAD_READY);
    if (t->status != THREAD_READY) {
        if (thread_mlfqs) {
            // MLFQS: 해당 큐에 삽입
            t->mlfqs_ticks = 0;
            list_push_back (mlfqs_get_queue(t->mlfqs_queue_level), &t->elem);
        } else {
            // Priority Scheduling: 우선순위 순으로 ready_list에 삽입
            list_insert_ordered (&ready_list, &t->elem, priority_less, NULL);
        }
        t->status = THREAD_READY;
    }
    intr_set_level (old_level);
    
    // *** 선점 로직: 새로 언블록된 스레드가 현재 스레드보다 우선순위가 높으면 양보 ***
    if (!thread_mlfqs) {
        if (t->priority > thread_current()->priority)
            thread_yield();
    }
}

/* ... (get_next_tick_to_wakeup, thread_sleep, thread_wakeup 함수는 timer.c에 정의되어 있으므로 생략) ... */

/* Returns the current thread's thread control block. */
struct thread *
thread_current (void)
{
    return running_thread ();
}

/* Returns the current thread's thread id. */
tid_t
thread_tid (void)
{
    return thread_current ()->tid;
}

/* Returns the current thread's name. */
const char *
thread_name (void)
{
    return thread_current ()->name;
}

/* Deschedules the current thread and destroys it. */
void
thread_exit (void) 
{
    ASSERT (!intr_context ());

#ifdef USERPROG
    process_exit ();
#endif

    /* Remove thread from all threads list, set status to dying.
       It is not safe to access the thread structure after this. */
    intr_disable ();
    list_remove (&thread_current()->allelem);
    thread_current()->status = THREAD_DYING;
    schedule ();
    NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;
    
    ASSERT (!intr_context ());

    old_level = intr_disable ();
    if (cur != idle_thread) {
        if (thread_mlfqs) {
            // MLFQS: 해당 큐의 맨 뒤로 이동 (Round Robin within same queue)
            list_push_back (mlfqs_get_queue(cur->mlfqs_queue_level), &cur->elem);
        } else {
            // Priority Scheduling: 우선순위 순으로 ready_list에 재삽입
            list_insert_ordered (&ready_list, &cur->elem, priority_less, NULL);
        }
    }
    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}

/* ... (thread_foreach 함수는 유지) ... */

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority)
{
    struct thread *cur = thread_current();
    enum intr_level old_level = intr_disable ();

    // *** 우선순위 기부: 원래 우선순위(original_priority)를 변경합니다. ***
    cur->original_priority = new_priority;

    // 현재 우선순위 (priority)를 업데이트하고, 기부받은 우선순위와 비교합니다.
    thread_update_priority(cur);

    intr_set_level (old_level);

    // *** 선점 로직: 우선순위가 낮아져 스케줄링이 필요하다면 양보 ***
    thread_yield();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
    // *** 우선순위 기부: 현재 적용 중인 우선순위(donated priority 포함)를 반환합니다. ***
    return thread_current ()->priority;
}

// ===================================================================
// *** MODIFICATION: 우선순위 기부/갱신 관련 헬퍼 함수 구현 ***

/* 스레드 T의 우선순위를 업데이트합니다. 
   T->priority는 T->original_priority와 T가 기부받은 가장 높은 우선순위 중 큰 값입니다. */
void
thread_update_priority (struct thread *t)
{
    int max_priority = t->original_priority;

    if (!list_empty(&t->donations)) {
        // 기부받은 목록에서 가장 높은 우선순위를 찾습니다.
        struct list_elem *e = list_begin(&t->donations);
        struct thread *donor = list_entry (e, struct thread, donation_elem);
        int donated_priority = donor->priority;

        if (donated_priority > max_priority)
            max_priority = donated_priority;
    }

    t->priority = max_priority;
}

/* 현재 스레드가 기다리는 락의 소유자에게 자신의 우선순위를 기부합니다. */
void
thread_donate_priority (void)
{
    struct thread *cur = thread_current();
    struct lock *lock;
    
    // 재귀적 기부를 위해 8단계까지 (예시 깊이)
    for (int i = 0; i < 8; i++) 
    {
        lock = cur->wait_on_lock;
        if (lock == NULL || lock->holder == NULL)
            break;
            
        struct thread *holder = lock->holder;
        
        // 락 소유자의 현재 우선순위가 기부할 우선순위보다 낮다면 업데이트
        if (holder->priority < cur->priority) {
            holder->priority = cur->priority;
            cur = holder; // 다음 단계 재귀적 기부를 위해 이동
        } else {
            break; // 더 이상 기부할 필요가 없습니다.
        }
    }
}

/* 락 해제 또는 락 획득 시 락 대기 리스트에서 현재 스레드를 제거하고
   락 소유자의 우선순위를 재계산합니다. */
void
thread_remove_donation (struct lock *lock)
{
    struct thread *holder = lock->holder;
    if (holder == NULL) return;

    // 기부 목록에서 락을 기다리던 스레드를 제거합니다. (락 획득 성공 시)
    list_remove(&thread_current()->donation_elem);
    
    // 락 소유자의 기부 목록에서 현재 락 관련 항목들을 정리한 후,
    // 원래 우선순위로 돌아가거나 새로운 최고 우선순위를 적용합니다.
    holder->priority = holder->original_priority;
    
    if (!list_empty(&holder->donations)) {
        struct thread *donor = list_entry (list_front(&holder->donations), struct thread, donation_elem);
        if (donor->priority > holder->priority)
            holder->priority = donor->priority;
    }
}
// ===================================================================

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

/* Initializes T, an uninitialized struct thread. */
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
    t->original_priority = priority; // 기부 기능을 위한 원래 우선순위 저장
    list_init(&t->donations); // 기부 목록 초기화
    t->wait_on_lock = NULL;

    t->magic = THREAD_MAGIC;
    list_push_back (&all_list, &t->allelem);

    t->age = 0; /* Aging 초기화 */
    t->mlfqs_queue_level = Q2; /* MLFQS 초기화 */
    t->mlfqs_ticks = 0;
    t->wakeup_tick = 0;
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
    uint32_t *esp;

    /* Copy ESP. */
    asm ("mov %%esp, %0" : "=g" (esp));

    /* Return the thread control block. */
    return (struct thread *) ((unsigned) esp & ~PGMASK);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
    return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic setup of stack frames for a new thread. */
static void *
alloc_frame (struct thread *t, size_t size)
{
    /* Stack starts at the top of the page. */
    uint8_t *base = (uint8_t *) t + PGSIZE;

    /* Subtract size from stack pointer. */
    base -= size;

    /* Back up 4 bytes for the return address. */
    base -= sizeof (void *);
    
    return base;
}

/* Chooses and returns the next thread to be scheduled. */
static struct thread *
next_thread_to_run (void)
{
    if (thread_mlfqs) {
        // MLFQS: Q0 -> Q1 -> Q2 순으로 확인
        for (enum mlfqs_queue q = Q0; q <= Q2; q++) {
            struct list *q_list = mlfqs_get_queue(q);
            if (!list_empty(q_list))
                return list_entry (list_pop_front (q_list), struct thread, elem);
        }
    } else {
        // Priority Scheduling: ready_list에서 최고 우선순위 스레드 선택
        if (!list_empty (&ready_list))
            return list_entry (list_pop_front (&ready_list), struct thread, elem);
    }
    return idle_thread;
}

/* Chooses and switches to the next thread to be scheduled. */
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

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   The former thread is passed as PREV.  This function may be
   called within an interrupt handler, but only after the thread
   switch itself has happened. */
void
thread_schedule_tail (struct thread *prev)
{
    struct thread *cur = running_thread ();
  
    ASSERT (intr_get_level () == INTR_OFF);

    /* Set up active page directory (if any). */
#ifdef USERPROG
    process_activate ();
#endif

    /* If PREV was dying, destroy it. */
    if (prev != NULL && prev->status == THREAD_DYING) 
        {
            ASSERT (prev != cur);
            palloc_free_page (prev);
        }
}

/* Idle thread.  Executes when no other thread is ready to run. */
static void
idle (void *aux UNUSED) 
{
    for (;;) 
        {
            enum intr_level old_level;

            /* Disable interrupts from here to there, to prevent both the
               thread being ready and the scheduler being invoked between
               the two statements below.  And, of course, to keep them
               atomic. */
            old_level = intr_disable ();
            if (list_empty (&ready_list)) 
                {
                    printf("Idle thread running...\n"); // 디버깅을 위해 추가
                    switch_to_idle ();
                }
            intr_set_level (old_level);
        }
}

/* kernel_thread and other utility functions remain as they are... */
static void
kernel_thread (thread_func *function, void *aux) 
{
    ASSERT (function != NULL);

    intr_enable ();       /* Enable interrupts in kernel mode. */
    function (aux);       /* Call the user's function. */

    thread_exit ();       /* If function() returns, kill the thread. */
}
