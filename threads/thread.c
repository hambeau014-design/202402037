/* thread.h */

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

// ===================================================================
// *** MODIFICATION: MLFQS 및 Aging 상수/Enum 정의 (기존 코드를 유지하며) ***

// Simplified MLFQS 큐 레벨 정의
enum mlfqs_queue 
{ 
    Q0, // Highest Priority, Time Slice 2
    Q1, // Medium Priority, Time Slice 4
    Q2  // Lowest Priority, Time Slice 8
};

// Simplified MLFQS 시간 슬라이스 상수 정의
#define MLFQS_Q0_SLICE 2
#define MLFQS_Q1_SLICE 4
#define MLFQS_Q2_SLICE 8

// 에이징 및 MLFQS 승급 기준 정의
#define AGING_TICKS 20 

// ===================================================================

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
        0 kB +---------------------------------+
             |              ...              |
             |              ...              |
             |              ...              |
             |       struct thread,          |
             |        all other data         |
             +---------------------------------+

   The first thing stored in the struct thread is the `magic'
   member, to help detect stack overflow.  Think of it as a
   mini-security check.

   The `magic' member is also used to detect uninitialized
   threads, so that a thread stack page is not accidentally
   passed to palloc_free_page(). */
struct thread
{
    /* Owned by thread.c. */
    tid_t tid;                 /* Thread identifier. */
    enum thread_status status; /* Thread state. */
    char name[16];             /* Name (for debugging purposes). */
    uint8_t *stack;            /* Saved stack pointer. */
    int priority;              /* Priority. */
    struct list_elem allelem;  /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem; /* List element. */
    
    // ===================================================================
    // *** MODIFICATION: Aging, MLFQS, Priority Donation 멤버 추가 ***
    int age;                   /* Age for aging and priority promotion (ticks waiting). */
    enum mlfqs_queue mlfqs_queue_level; /* Current MLFQS queue level. */
    int mlfqs_ticks;           /* Ticks used in the current time slice. */
    
    // Priority Donation
    int original_priority;     /* Thread's priority without donation. */
    struct lock *wait_on_lock; /* Lock thread is waiting for (NULL if none). */
    struct list_elem donation_elem; /* Element for the list of threads donating priority. */
    struct list donations;     /* List of donation_elem from threads waiting on its lock. */
    // ===================================================================
    
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir; /* Page directory. */
#endif

    /* For timer_sleep() */
    int64_t wakeup_tick;

    /* Owned by thread.c. */
    unsigned magic; /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

int64_t get_next_tick_to_wakeup (void);
void thread_sleep (int64_t);
void thread_wakeup (int64_t);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

// ===================================================================
// *** MODIFICATION: 우선순위 비교 함수 및 기부 관련 함수 선언 ***
bool priority_less (const struct list_elem *a, const struct list_elem *b, void *aux);
void thread_update_priority (struct thread *t);
void thread_donate_priority (void);
void thread_remove_donation (struct lock *lock);
void thread_set_priority (int new_priority);
int thread_get_priority (void);
// ===================================================================

#endif /* threads/thread.h */
