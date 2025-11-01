#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"

/* Thread identifier type. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Minimum priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Maximum priority. */

struct lock; /* fwd */

enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* (선택) 간단 에이징/MLFQS 훅 */
enum mlfqs_queue { Q0, Q1, Q2 };

struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* 현재 적용 우선순위 (effective). */
    int original_priority;              /* 기부 복원용 원래 우선순위. */
    struct list donations;              /* 나에게 우선순위 기부한 스레드 목록. */
    struct list_elem donation_elem;     /* 다른 스레드 donations 리스트의 elem. */
    struct lock *wait_on_lock;          /* 내가 기다리는 lock (도네이션용). */

    /* ready/sleep/all 리스트용 요소 */
    struct list_elem elem;              /* List element. */

    /* Sleep 지원 */
    int64_t wakeup_tick;                /* 깨어날 tick. */

    /* (선택) 에이징/MLFQS */
    int age;                            /* Aging 카운터. */
    enum mlfqs_queue mlfqs_queue_level; /* Q0/Q1/Q2 (미사용시 Q2 유지). */
    int mlfqs_ticks;                    /* 현재 큐에서 소비한 틱. */

#ifdef USERPROG
    uint32_t *pagedir;                  /* Page directory. */
#endif
    unsigned magic;                     /* Detects stack overflow. */
  };

/* 프로젝트 전반에서 쓰는 helper들 */
void thread_update_priority (struct thread *t);
void thread_donate_priority (void);
void thread_remove_donation (struct lock *lock);

tid_t thread_create (const char *name, int priority, thread_func *, void *aux);
void thread_init (void);
void thread_start (void);
void thread_tick (void);
void thread_print_stats (void);

void thread_block (void);
void thread_unblock (struct thread *);

const char *thread_name (void);
struct thread *thread_current (void);
tid_t thread_tid (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

void thread_foreach (thread_action_func *, void *);

/* 스케줄링 API */
void thread_set_priority (int);
int thread_get_priority (void);

#endif /* threads/thread.h */
