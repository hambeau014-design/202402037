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
#include "devices/timer.h" // thread_cmp_wake_up_tick 선언을 포함하기 위해 추가
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Global State */
static struct list ready_list;      /* List of threads ready to run. */
static struct list all_list;       /* List of all threads. */
static struct thread *idle_thread; /* The idle thread. */
static struct thread *initial_thread; /* The thread that executes main(). */
static struct lock tid_lock;       /* Lock for tid allocator. */
int thread_ticks;                  /* # of timer ticks since last yield. */

/* MLFQS Global Variables */
struct list mlfq[3];               /* Multi-level feedback queues (Q0, Q1, Q2). */
bool thread_mlfqs;                 /* MLFQS control flag. */

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* MLFQ Constants */
#define TIME_SLICE 4 // 기존 라운드 로빈 TIME_SLICE
#define TIME_SLICE_Q0 2
#define TIME_SLICE_Q1 4
#define TIME_SLICE_Q2 8
#define AGE_LIMIT 20

/* Function declarations */
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

/* Priority and Donation Functions */
bool thread_cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
bool thread_cmp_wake_up_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED); // Timer Sleep용
int thread_get_max_ready_priority(void);
void thread_donate_priority(struct thread *t, int priority);
void thread_remove_lock(struct lock *lock);
void thread_recalculate_priority(struct thread *t);

/* Scheduling and Aging Functions */
void mlfq_update(void);
void aging_ready_threads(void);

/* Initializes the threading system by transforming the code that's currently
   running into a thread. */
void
thread_init (void)
{
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);
    list_init (&ready_list);
    list_init (&all_list);

    list_init(&mlfq[0]);
    list_init(&mlfq[1]);
    list_init(&mlfq[2]);

    /* Set up a thread structure for the running thread. */
    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}

/* Starts the scheduler by enabling interrupts and switching to the idle thread. */
void
thread_start (void)
{
    /* Create the idle thread. */
    struct semaphore idle_started;
    sema_init (&idle_started, 0);
    thread_create ("idle", PRI_MIN, idle, &idle_started);

    /* Start preemptive thread scheduling. */
    intr_enable ();

    /* Wait for the idle thread to initialize idle_thread. */
    sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Interrupts must be disabled. */
void
thread_tick (void)
{
    struct thread *t = thread_current ();
    
    /* Update statistics. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    /* MLFQS/Aging Logic */
    if (thread_mlfqs) {
        mlfq_update();
    }
    else {
        // Priority Scheduling 모드: 대기 스레드 에이징
        aging_ready_threads(); 
    }
    
    /* Enforce preemption. */
    if (thread_mlfqs == false) {
        if (++thread_ticks >= TIME_SLICE)
            intr_yield_on_return ();
    }
    // MLFQS 모드의 타임 슬라이스 관리는 mlfq_update() 내에서 intr_yield_on_return()을 호출합니다.
}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given PRIORITY,
   which executes FUNCTION passing AUX as the argument, and adds it to
   the ready queue. Returns the thread identifier for the new thread, or
   TID_ERROR if creation fails. */
tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux)
{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;
    enum intr_level old_level;

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
    sf->eip = (void (*) (void)) thread_schedule_tail;
    sf->esi = sf->edi = sf->ebp = 0;

    /* Add to run queue. */
    old_level = intr_disable ();
    thread_unblock (t);

    /* 선점 (새로운 스레드가 현재 스레드보다 우선순위가 높으면 yield) */
    if (t->priority > thread_current()->priority)
        thread_yield();

    intr_set_level (old_level);

    return tid;
}

/* Puts the current thread to sleep. It will not be scheduled again until
   awoken by thread_unblock(). */
void
thread_block (void)
{
    thread_current ()->status = THREAD_BLOCKED;
    schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state. */
void
thread_unblock (struct thread *t)
{
    enum intr_level old_level;
    ASSERT (is_thread (t));

    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED);
    
    t->status = THREAD_READY;
    
    if (thread_mlfqs == false) {
        /* Priority Scheduling: 우선순위 순으로 삽입 */
        list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);

        /* 선점 (Unblock된 스레드가 현재 스레드보다 우선순위가 높으면 yield) */
        if (t->priority > thread_current()->priority)
            thread_yield(); // thread_yield는 intr_context()를 확인하므로 안전
    }
    else {  
        /* MLFQS Scheduling: 큐에 push_back */
        if (t->queue_level == -1) {
            t->queue_level = 0;
            t->age[0] = t->age[1] = t->age[2] = 0;
        }
        list_push_back(&mlfq[t->queue_level], &t->elem);
        
        // MLFQS 선점: unblock된 스레드가 더 높은 우선순위 큐(작은 인덱스)에 있을 때
        if (t->queue_level < thread_current()->queue_level) {
             intr_yield_on_return();
        }
    }

    intr_set_level (old_level);
}

/* Returns the current thread's thread id. */
tid_t
thread_tid (void)
{
    return thread_current ()->tid;
}

/* Returns the current thread. */
struct thread *
thread_current (void)
{
    return running_thread ();
}

/* Returns the name of the running thread. */
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

    /* Remove thread from all threads list, set our status to dying,
       and schedule another process.  That process will destroy us
       when it calls thread_schedule_tail(). */
    intr_disable ();
    list_remove (&thread_current()->allelem);
    thread_current ()->status = THREAD_DYING;
    schedule ();
    NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and may be
   scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (!intr_context ());

    old_level = intr_disable ();
    if (cur != idle_thread) {
        if (thread_mlfqs == false) {
            /* Priority Scheduling: 우선순위 순으로 삽입 */
            list_insert_ordered (&ready_list, &cur->elem, thread_cmp_priority, NULL);
        } else {
            /* MLFQS Scheduling: 현재 큐에 push_back */
            if (cur->queue_level == -1)
                cur->queue_level = 0;
            list_push_back (&mlfq[cur->queue_level], &cur->elem);
        }
    }
        
    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}

/* Performs some operation on thread t, given auxiliary data AUX. */
void
thread_foreach (thread_action_func *action, void *aux)
{
    struct list_elem *e;

    intr_disable ();
    for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
        {
            struct thread *t = list_entry (e, struct thread, allelem);
            action (t, aux);
        }
    intr_enable ();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
    return thread_current ()->priority;
}

/* Sets the current thread's base priority to NEW_PRIORITY. 
   If priority donation is active, the effective priority is recalculated. */
void
thread_set_priority (int new_priority) 
{
    enum intr_level old_level = intr_disable();
    struct thread *cur = thread_current();
    
    if (thread_mlfqs == false) {
        // 1. 원래 우선순위(original_priority)를 먼저 업데이트
        cur->original_priority = new_priority;
        
        // 2. 새로운 유효 우선순위를 계산하고 cur->priority에 반영
        thread_recalculate_priority(cur);

        // 3. 선점 로직 (priority-change 테스트 통과를 위한 핵심)
        // 새로운 우선순위가 ready_list의 최고 우선순위보다 낮아지면 yield (선점)
        if (!list_empty(&ready_list) && cur->priority < thread_get_max_ready_priority())
            thread_yield();
    }
    intr_set_level(old_level);
}

/* Sets the current thread's nice value to NEW_NICE. */
int
thread_get_nice (void)
{
    // MLFQS 구현에 따라 구현 필요
    return 0;
}

/* Returns the current thread's recent_cpu value. */
void
thread_set_nice (int new_nice)
{
    // MLFQS 구현에 따라 구현 필요
}

/* Returns the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void)
{
    // MLFQS 구현에 따라 구현 필요
    return 0;
}

/* Returns the current load_avg. */
int
thread_get_load_avg (void)
{
    // MLFQS 구현에 따라 구현 필요
    return 0;
}

/* Idle thread.  Executes when no other thread is ready to run. */
static void
idle (void *aux UNUSED) 
{
    for (;;) 
        {
            intr_disable ();
            thread_block ();
            intr_enable ();
        }
}

/* C code for the kernel_thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
    ASSERT (function != NULL);

    intr_enable ();       /* Enable interrupts in kernel_thread. */
    function (aux);       /* Execute the thread function. */
    thread_exit ();       /* If function() returns, call thread_exit(). */
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
    uint32_t *esp;

    /* Tidy up to allow us to express 'const struct thread *t' in C. */
    asm ("mov %%esp, %0" : "=g" (esp));

    return (struct thread *) ((uint32_t) esp & 0xfffff000);
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
    t->stack = (uint8_t *)t + PGSIZE;
    t->priority = priority;
    t->magic = THREAD_MAGIC;

    /* 🚨 [추가] Donation/Sleep 필드 초기화 */
    t->original_priority = priority;
    t->wait_on_lock = NULL;
    list_init (&t->holding_locks);
    t->wake_up_tick = 0; // Timer Sleep용
    
    /* MLFQ / bookkeeping defaults */
    t->queue_level = -1;
    t->nice = 0;
    t->recent_cpu = 0;
    t->age[0] = t->age[1] = t->age[2] = 0;
    
    list_push_back (&all_list, &t->allelem);
}

/* Allocates a frame for an interrupt or exception handler. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
    /* Stack starts in the user's stack page and grows down. */
    ASSERT (is_thread (t));
    ASSERT (size % 4 == 0);

    t->stack -= size;
    return t->stack;
}

/* Chooses and transitions to the next thread to be scheduled. */
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

/* Returns the highest-priority thread to run, or the idle thread if none
   are ready. */
static struct thread *
next_thread_to_run (void)
{
    if (thread_mlfqs == false) {
        /* Priority Scheduling: ready_list의 가장 높은 우선순위 스레드를 pop */
        if (list_empty (&ready_list))
            return idle_thread;
        else
            return list_entry (list_pop_front (&ready_list), struct thread, elem);
    }
    else {  
        /* MLFQS Scheduling: Q0 -> Q1 -> Q2 순으로 pop */
        for (int i = 0; i < 3; i++) {
            if (!list_empty (&mlfq[i])) {
                return list_entry (list_pop_front (&mlfq[i]), struct thread, elem);
            }
        }
        return idle_thread;
    }
}

/* Completes a thread switch by activating the new page tables,
   and, if necessary, destroying the previous thread. */
void
thread_schedule_tail (struct thread *prev) 
{
    struct thread *cur = running_thread ();
    
    ASSERT (intr_get_level () == INTR_ON);

    /* Mark us as running. */
    cur->status = THREAD_RUNNING;

    /* Activate the new thread's page tables. */
#ifdef USERPROG
    process_activate ();
#endif

    /* If the thread we switched from is dying, destroy it.
       This must happen after activating the new thread's page tables,
       because the dying thread's page tables become useless as soon as
       they are deactivated. */
    if (prev != NULL && prev->status == THREAD_DYING) 
        {
            ASSERT (prev != cur);
            palloc_free_page (prev);
        }
}

/* Allocates a new thread ID (tid). */
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

/* Priority-based list comparison functions */

/* thread_cmp_priority - 우선순위 비교 함수 (높은 우선순위가 앞으로) */
bool
thread_cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
   struct thread *t_a = list_entry(a, struct thread, elem);
   struct thread *t_b = list_entry(b, struct thread, elem);
   return t_a->priority > t_b->priority;
}

/* thread_cmp_wake_up_tick - 깨어날 시간(wake_up_tick)을 기준으로 비교 (작은 값이 앞으로) */
bool
thread_cmp_wake_up_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
    struct thread *t_a = list_entry(a, struct thread, elem);
    struct thread *t_b = list_entry(b, struct thread, elem);
    return t_a->wake_up_tick < t_b->wake_up_tick;
}


/* thread_get_max_ready_priority - ready_list의 최고 우선순위 반환 (선점 로직용) */
int
thread_get_max_ready_priority(void)
{
    if (list_empty(&ready_list))
        return PRI_MIN - 1; 
    
    struct thread *t = list_entry(list_front(&ready_list), struct thread, elem);
    return t->priority;
}

/* thread_recalculate_priority - 현재 보유한 락과 original_priority를 기준으로 유효 우선순위 재계산 */
void
thread_recalculate_priority(struct thread *t)
{
    if (thread_mlfqs == true) return;
    
    int new_effective_priority = t->original_priority;
    
    // 보유한 모든 락의 최대 기부 우선순위를 찾습니다.
    if (!list_empty(&t->holding_locks)) {
        struct list_elem *e;
        int max_donated_priority = PRI_MIN;
        
        for (e = list_begin(&t->holding_locks); e != list_end(&t->holding_locks); e = list_next(e)) {
            struct lock *l = list_entry(e, struct lock, elem);
            if (l->max_priority > max_donated_priority) {
                max_donated_priority = l->max_priority;
            }
        }
        
        // 최종 유효 우선순위 = max(original_priority, max_donated_priority)
        if (max_donated_priority > new_effective_priority) {
            new_effective_priority = max_donated_priority;
        }
    }
    
    // 우선순위가 변경된 경우에만 업데이트 및 리스트 위치 갱신
    if (t->priority != new_effective_priority) {
        t->priority = new_effective_priority;

        // ready 상태였고 우선순위가 변경되었다면, ready_list에서 위치를 갱신
        if (t->status == THREAD_READY) {
            list_remove(&t->elem);
            list_insert_ordered(&ready_list, &t->elem, thread_cmp_priority, NULL);
        }
    }
}

/* thread_donate_priority - t에게 priority를 기부하고, t가 대기하는 락의 holder에게 연쇄 기부 */
void
thread_donate_priority(struct thread *t, int priority)
{
    if (thread_mlfqs == true) return;
    
    if (t->priority < priority) {
        t->priority = priority;
        
        // 연쇄 기부: t가 락을 기다리고 있다면, 락의 max_priority를 갱신하고 holder에게 기부
        if (t->wait_on_lock != NULL) {
            // 락의 max_priority를 갱신
            if (t->wait_on_lock->max_priority < priority) {
                t->wait_on_lock->max_priority = priority;
            }
            
            // 락 보유자가 있다면 재귀적으로 기부
            if (t->wait_on_lock->holder != NULL) {
                 thread_donate_priority(t->wait_on_lock->holder, priority);
            }
        }
        
        // ready 상태라면 리스트에서 제거 후 재삽입하여 정렬 위치 갱신
        if (t->status == THREAD_READY) {
            list_remove(&t->elem);
            list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);
        }
    }
}

/* thread_remove_lock - 락 해제 시 우선순위 회수 및 갱신 */
void
thread_remove_lock(struct lock *lock)
{
    struct thread *cur = thread_current();
    enum intr_level old_level = intr_disable();

    // 락 해제 후 현재 스레드의 유효 우선순위 재계산
    thread_recalculate_priority(cur);
    
    // 우선순위가 낮아진 경우 선점 유도
    if (cur->priority < thread_get_max_ready_priority() && !intr_context())
        thread_yield();
    
    intr_set_level(old_level);
}

/* aging_ready_threads - Priority Aging (우선순위 스케줄링 모드에서만 사용) */
void
aging_ready_threads(void)
{
    if (thread_mlfqs == false) {
        struct list_elem *e = list_begin (&ready_list);
        while (e != list_end (&ready_list)) {
            struct list_elem *next = list_next(e);
            struct thread *t = list_entry (e, struct thread, elem);

            t->age[0]++; // age[0]을 일반 age 카운터로 사용
            
            if (t->age[0] >= AGE_LIMIT) {
                t->age[0] = 0;
                // PRI_MAX까지 승급 허용 (priority-aging 테스트 통과를 위해)
                if (t->priority < PRI_MAX) { 
                    t->priority++;
                    
                    // 우선순위가 바뀌었으므로 리스트에서 제거 후 재삽입
                    list_remove(&t->elem);
                    list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);
                    
                    // 우선순위가 높아졌다면 즉시 선점 유도
                    if (t->priority > thread_current()->priority)
                        intr_yield_on_return(); 
                }
            }
            e = next;
        }
    }
}


/* mlfq_update - MLFQS 강등 및 에이징 승급 */
void
mlfq_update(void)
{
    struct thread *cur = thread_current();

    if (cur != idle_thread) {
        cur->recent_cpu++;
        int slice_limit = (cur->queue_level == 0) ? TIME_SLICE_Q0
                         : (cur->queue_level == 1) ? TIME_SLICE_Q1
                         : TIME_SLICE_Q2;

        /* 1. 타임 슬라이스 소모 시 강등 및 yield */
        if (cur->recent_cpu >= slice_limit) {
            if (cur->queue_level < 2)
                cur->queue_level++;
            cur->recent_cpu = 0;
            
            // 강등 후 선점
            intr_yield_on_return (); 
            return;
        }
    }

    /* 2. 대기 중인 스레드 에이징 및 승급 */
    for (int i = 0; i < 3; i++) {
        struct list_elem *e = list_begin (&mlfq[i]);
        while (e != list_end (&mlfq[i])) {
            struct list_elem *next = list_next (e);
            struct thread *t = list_entry (e, struct thread, elem);
            
            t->age[i]++;

            if (t->age[i] >= AGE_LIMIT && t->queue_level > 0) {
                // 승급
                list_remove (&t->elem);
                t->queue_level--;
                t->age[0] = t->age[1] = t->age[2] = 0;  // 모든 age 카운터 리셋
                list_push_back (&mlfq[t->queue_level], &t->elem);

                // 승급으로 Q0에 스레드가 추가되었고, 현재 스레드보다 우선순위가 높다면 선점
                if (t->queue_level < cur->queue_level) {
                    intr_yield_on_return();
                }
            }
            e = next;
        }
    }
}
