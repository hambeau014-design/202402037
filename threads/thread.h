#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* Thread identifier type. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)

/* Thread priorities. */
#define PRI_MIN 0
#define PRI_DEFAULT 31
#define PRI_MAX 63

/* MLFQS(단순 3단 큐) 레벨 (필요시 사용; threads 단계에서는 flag만 존재해도 무방) */
enum mlfqs_queue { Q0 = 0, Q1 = 1, Q2 = 2 };

enum thread_status {
  THREAD_RUNNING,     /* Running thread. */
  THREAD_READY,       /* Not running but ready to run. */
  THREAD_BLOCKED,     /* Waiting for an event to trigger. */
  THREAD_DYING        /* About to be destroyed. */
};

struct lock;                 /* fwd decl */
typedef void thread_func (void *aux);

/* Thread control block. (Project 1 호환) */
struct thread
  {
    /* --- 기본 필드 (Pintos 표준) --- */
    tid_t tid;                      /* Thread identifier. */
    enum thread_status status;      /* Thread state. */
    char name[16];                  /* Name (for debugging purposes). */
    uint8_t *stack;                 /* Saved stack pointer. */
    int priority;                   /* Effective priority. */
    struct list_elem allelem;       /* List element for all threads list. */

    /* --- 스케줄링: ready/sleep 대기열 링크 --- */
    struct list_elem elem;          /* List element. */

    /* --- 우선순위 도네이션 --- */
    int original_priority;          /* Base priority before donation. */
    struct list donations;          /* List of donors (struct thread via donation_elem). */
    struct list_elem donation_elem; /* As an element in someone else’s donations list. */
    struct lock *wait_on_lock;      /* Lock I am waiting on (donation target). */

    /* --- FIFO RR 타이브레이커 & 에이징 --- */
    int64_t ready_stamp;            /* FIFO for equal priority in ready queue. */
    int age;                        /* Aging counter while READY. */

    /* --- Sleep(타이머) --- */
    int64_t wakeup_tick;            /* Tick to wake at. */

    /* --- 단순 MLFQS(선택 사용) --- */
    enum mlfqs_queue qlevel;        /* Q0 -> Q1 -> Q2 */
    int run_ticks_in_level;         /* Consumed ticks in current level */

    /* Detects stack overflow. */
    unsigned magic;                 /* Detects stack overflow. */
  };
typedef void thread_action_func (struct thread *t, void *aux);

/* 전역 플래그: -mlfqs 사용 여부 (threads 단계에서는 false가 기본) */
extern bool thread_mlfqs;

/* Thread subsystem. */
void thread_init (void);
void thread_start (void);
void thread_tick (void);
void thread_print_stats (void);

/* Basic thread functions. */
typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *aux);

void thread_block (void);
void thread_unblock (struct thread *);

const char *thread_name (void);
struct thread *thread_current (void);
tid_t thread_tid (void);
void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Iteration. */
void thread_foreach (thread_action_func *, void *aux);

/* Priority. */
void thread_set_priority (int new_priority);
int  thread_get_priority (void);

/* Donation helpers. */
void thread_update_priority (struct thread *t);
void thread_donate_priority (void);
void thread_remove_donation (struct lock *lock);

/* Sleep helper (timer.c에서 사용). */
void thread_sleep_until (int64_t wake_tick);

#endif /* threads/thread.h */
