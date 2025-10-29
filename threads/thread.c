/* threads/thread.c
   MLFQ (3-level) + Aging + Preemption implementation for Pintos
   Replace your existing threads/thread.c with this file (or merge carefully).
*/

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

/* Ready lists: MLFQ with 3 levels */
#define MLFQ_LEVELS 3
static struct list mlfq[MLFQ_LEVELS];

/* List of all processes. */
static struct list all_list;

/* Idle thread and initial thread */
static struct thread *idle_thread;
static struct thread *initial_thread;

/* Tid lock */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
{
    void *eip;
    thread_func *function;
    void *aux;
};

/* Statistics. */
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

/* Scheduling. */
#define TIME_SLICE 4 /* not used directly for MLFQ levels, but for compatibility */
static unsigned thread_ticks;

/* Time slice per MLFQ level (in ticks) */
#define TIME_SLICE_Q0 1
#define TIME_SLICE_Q1 2
#define TIME_SLICE_Q2 4

/* Aging threshold (ticks waiting in ready queue) */
#define AGE_LIMIT 20

/* If false, use round-robin; if true, use MLFQ */
bool thread_mlfqs;

/* Forward declarations */
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

static void mlfq_tick(void);
static void mlfq_aging(void);

/* Initialize threading system. */
void
thread_init (void)
{
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);
    list_init (&all_list);

    for (int i = 0; i < MLFQ_LEVELS; i++)
        list_init (&mlfq[i]);

    /* Set up running thread structure. */
    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}

/* Start threads: create idle thread, enable interrupts. */
void
thread_start (void)
{
    struct semaphore idle_started;
    sema_init (&idle_started, 0);
    thread_create ("idle", PRI_MIN, idle, &idle_started);

    intr_enable ();
    sema_down (&idle_started);
}

/* Timer tick handler: update stats and perform MLFQ tick+aging */
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

    /* Common tick increment */
    thread_ticks++;

    /* If MLFQ enabled, update per-thread CPU usage / timeslice and aging */
    if (thread_mlfqs) {
        mlfq_tick();    /* update current thread ticks_in_queue and demote if necessary */
        mlfq_aging();   /* age waiting threads and promote if necessary */
    } else {
        /* Classic RR behavior */
        if (thread_ticks >= TIME_SLICE)
            intr_yield_on_return ();
    }
}

/* Print stats */
void
thread_print_stats (void)
{
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

/* Create a new kernel thread and add it to ready queue (MLFQ). */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;
    enum intr_level old_level;

    ASSERT (function != NULL);

    t = palloc_get_page (PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    init_thread (t, name, priority);
    tid = t->tid = allocate_tid ();

    /* Initialize MLFQ bookkeeping */
    t->queue_level = 0;        /* new threads start at highest queue */
    t->ticks_in_queue = 0;
    t->wait_ticks = 0;

    old_level = intr_disable ();

    /* Prepare stack frames */
    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void))kernel_thread;

    sf = alloc_frame (t, sizeof *sf);
    sf->eip = switch_entry;
    sf->ebp = 0;

    intr_set_level (old_level);

    /* Put thread into ready (mlfq) */
    thread_unblock (t);

    return tid;
}

/* Block current thread (must be called with interrupts off). */
void
thread_block (void)
{
    ASSERT (!intr_context ());
    ASSERT (intr_get_level () == INTR_OFF);

    thread_current ()->status = THREAD_BLOCKED;
    schedule ();
}

/* Unblock thread: put into MLFQ ready queue and handle preemption. */
void
thread_unblock (struct thread *t)
{
    enum intr_level old_level;
    ASSERT (is_thread (t));

    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED);

    t->status = THREAD_READY;

    /* If queue_level uninitialized, initialize to 0 */
    if (t->queue_level < 0)
        t->queue_level = 0;

    /* reset wait_ticks for aging */
    t->wait_ticks = 0;
    t->ticks_in_queue = 0;

    /* push to corresponding MLFQ level back (FIFO) */
    list_push_back (&mlfq[t->queue_level], &t->elem);

    /* Preemption: if unblocked thread is at higher-priority queue than current, request preempt.
       If we're in interrupt context, use intr_yield_on_return; otherwise, call thread_yield (which expects interrupts off). */
    struct thread *cur = thread_current ();
    if (t != idle_thread && cur != idle_thread) {
        if (t->queue_level < cur->queue_level) {
            if (intr_context ())
                intr_yield_on_return ();
            else {
                /* We are allowed to yield now (interrupts off) */
                /* Note: thread_yield will call schedule(), so interrupts must be off. */
                thread_yield ();
            }
        }
    }

    intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
    return thread_current ()->name;
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

/* Returns current tid. */
tid_t
thread_tid (void)
{
    return thread_current ()->tid;
}

/* Exit current thread. */
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

/* Yield CPU voluntarily: put current thread at end of its MLFQ queue. */
void
thread_yield (void)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (!intr_context ());

    old_level = intr_disable ();

    if (cur != idle_thread) {
        /* Put current thread to back of its current queue */
        if (cur->queue_level < 0)
            cur->queue_level = 0;
        cur->ticks_in_queue = 0; /* reset CPU consumption in this queue (will be counted when re-scheduled) */
        list_push_back (&mlfq[cur->queue_level], &cur->elem);
    }

    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}

/* Apply function to all threads (interrupts off). */
void
thread_foreach (thread_action_func *func, void *aux)
{
    struct list_elem *e;

    ASSERT (intr_get_level () == INTR_OFF);

    for (e = list_begin (&all_list); e != list_end (&all_list);
         e = list_next (e))
    {
        struct thread *t = list_entry (e, struct thread, allelem);
        func (t, aux);
    }
}

/* Set current thread's priority. For MLFQ we primarily use queue levels, but keep API. */
void
thread_set_priority (int new_priority)
{
    int old_priority = thread_current ()->priority;
    thread_current ()->priority = new_priority;

    /* Priority change might affect scheduling only in non-MLFQ case or if user uses priority field.
       For safety, if effective priority decreased, yield to allow other threads to run. */
    if (new_priority < old_priority)
        thread_yield ();
}

/* Get priority */
int
thread_get_priority (void)
{
    return thread_current ()->priority;
}

/* Not implemented nice/load_avg in this MLFQ simplified implementation */
void
thread_set_nice (int nice UNUSED) { }
int thread_get_nice (void) { return 0; }
int thread_get_load_avg (void) { return 0; }
int thread_get_recent_cpu (void) { return 0; }

/* Idle thread. */
static void
idle (void *idle_started_ UNUSED)
{
    struct semaphore *idle_started = idle_started_;
    idle_thread = thread_current ();
    sema_up (idle_started);

    for (;;)
    {
        intr_disable ();
        thread_block ();

        asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Kernel thread wrapper. */
static void
kernel_thread (thread_func *function, void *aux)
{
    ASSERT (function != NULL);

    intr_enable ();
    function (aux);
    thread_exit ();
}

/* Get running_thread using stack pointer. */
struct thread *
running_thread (void)
{
    uint32_t *esp;
    asm ("mov %%esp, %0" : "=g"(esp));
    return pg_round_down (esp);
}

/* Verify thread pointer. */
static bool
is_thread (struct thread *t)
{
    return t != NULL && t->magic == THREAD_MAGIC;
}

/* Initialize thread structure. */
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
    t->magic = THREAD_MAGIC;

    /* MLFQ bookkeeping defaults */
    t->queue_level = 0;
    t->ticks_in_queue = 0;
    t->wait_ticks = 0;

    list_push_back (&all_list, &t->allelem);
}

/* Alloc stack frame. */
static void *
alloc_frame (struct thread *t, size_t size)
{
    ASSERT (is_thread (t));
    ASSERT (size % sizeof (uint32_t) == 0);

    t->stack -= size;
    return t->stack;
}

/* Choose next thread to run from highest-priority non-empty queue. */
static struct thread *
next_thread_to_run (void)
{
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        if (!list_empty (&mlfq[i])) {
            struct list_elem *e = list_pop_front (&mlfq[i]);
            struct thread *t = list_entry (e, struct thread, elem);
            return t;
        }
    }
    return idle_thread;
}

/* Complete context switch, finish bookkeeping. */
void
thread_schedule_tail (struct thread *prev)
{
    struct thread *cur = running_thread ();

    ASSERT (intr_get_level () == INTR_OFF);

    cur->status = THREAD_RUNNING;

    /* Start new time slice counter */
    thread_ticks = 0;

#ifdef USERPROG
    process_activate ();
#endif

    if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
        ASSERT (prev != cur);
        palloc_free_page (prev);
    }
}

/* Core scheduler: pick next and switch. */
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

/* Allocate tid. */
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

/* MLFQ helper: update current running thread's ticks and demote if needed. */
static void
mlfq_tick(void)
{
    struct thread *cur = thread_current ();
    if (cur == idle_thread)
        return;

    /* Ensure queue_level is valid */
    if (cur->queue_level < 0)
        cur->queue_level = 0;

    cur->ticks_in_queue++;

    int slice_limit = (cur->queue_level == 0) ? TIME_SLICE_Q0
                      : (cur->queue_level == 1) ? TIME_SLICE_Q1
                      : TIME_SLICE_Q2;

    if (cur->ticks_in_queue >= slice_limit) {
        /* Demote if possible */
        if (cur->queue_level < MLFQ_LEVELS - 1)
            cur->queue_level++;
        cur->ticks_in_queue = 0;

        /* Voluntary yield to let scheduler pick next thread */
        /* If interrupts on, use intr_yield_on_return to schedule on return from interrupt */
        if (intr_context ())
            intr_yield_on_return ();
        else
            thread_yield ();
    }
}

/* MLFQ aging: increment wait_ticks for threads in ready queues and promote if aged. */
static void
mlfq_aging(void)
{
    enum intr_level old_level = intr_disable ();

    for (int level = 1; level < MLFQ_LEVELS; level++) {
        struct list_elem *e = list_begin (&mlfq[level]);
        while (e != list_end (&mlfq[level])) {
            struct list_elem *next = list_next (e);
            struct thread *t = list_entry (e, struct thread, elem);

            t->wait_ticks++;
            if (t->wait_ticks >= AGE_LIMIT) {
                /* promote to higher priority queue (lower level number) */
                list_remove (&t->elem);
                if (t->queue_level > 0)
                    t->queue_level--;
                t->wait_ticks = 0;
                t->ticks_in_queue = 0;
                list_push_back (&mlfq[t->queue_level], &t->elem);

                /* If the promoted thread is now higher priority than current, request preempt */
                struct thread *cur = thread_current ();
                if (t != idle_thread && cur != idle_thread && t->queue_level < cur->queue_level) {
                    if (intr_context ())
                        intr_yield_on_return ();
                    else
                        thread_yield ();
                }

                /* after removal, continue iteration from next (we already set next) */
            }
            e = next;
        }
    }

    intr_set_level (old_level);
}
