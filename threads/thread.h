#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h" // struct lock 사용을 위해 추가

/* States in a thread's life cycle. */
enum thread_status
{
    THREAD_RUNNING, /* Running thread. */
    THREAD_READY,   /* Not running but ready to run. */
    THREAD_BLOCKED, /* Waiting for an event to trigger. */
    THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */

/* A kernel thread or user process. (중략) */

struct thread
{
    /* Owned by thread.c. */
    tid_t tid;              /* Thread identifier. */
    enum thread_status status; /* Thread state. */
    char name[16];           /* Name (for debugging purposes). */
    uint8_t *stack;         /* Saved stack pointer. */
    int priority;           /* Current effective priority (may be donated). */

    /* 🚨 [Priority Donation Fields] 🚨 */
    int original_priority;  /* Base priority set by user/nice value. */
    struct lock *wait_on_lock; /* Lock the thread is currently waiting on. */
    struct list holding_locks; /* List of locks the thread holds. */
    
    /* 🚨 [Timer Sleep Field] 🚨 */
    int64_t wake_up_tick;   /* Ticks when the thread should wake up. */

    /* MLFQ fields */
    int queue_level;        /* 0 (highest) .. 2 (lowest). -1 = not initialized */
    int nice;               /* Nice value for MLFQS (usually -20 to 20). */
    int recent_cpu;         /* Recent CPU usage (fixed-point arithmetic). */
    // 🚨 [MLFQS/Aging Field] thread.c의 로직에 맞게 배열 유지
    int age[3];             /* Aging counter for MLFQS queues. */
    
    struct list_elem allelem; /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;  /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;      /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;         /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler. (중략) */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */
