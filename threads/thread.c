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
