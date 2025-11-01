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

#define THREAD_MAGIC 0xcd6abf4b

/* ready lists: 일반 우선순위용 하나, MLFQS용 3단계 큐 */
static struct list ready_list;
static struct list ready_list_q0;
static struct list ready_list_q1;
static struct list ready_list_q2;

static struct list all_list;
static struct list sleep_list;
static int64_t next_tick_to_wakeup = INT64_MAX;

static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

struct kernel_thread_frame 
{
    void *eip;
    thread_func *function;
    void *aux;
};

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define TIME_SLICE 4
static unsigned thread_ticks;

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

/* ====================== 비교 함수들 ====================== */

/* ready_list / semaphore waiters 용 비교 (thread->elem 기준) */
bool
compare_thread_priority_elem (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux UNUSED)
{
    const struct thread *ta = list_entry (a, struct thread, elem);
    const struct thread *tb = list_entry (b, struct thread, elem);
    return ta->priority > tb->priority;
}

/* donations 리스트용 비교 (thread->donation_elem 기준) */
bool
compare_thread_priority_donation (const struct list_elem *a,
                                  const struct list_elem *b,
                                  void *aux UNUSED)
{
    const struct thread *ta = list_entry (a, struct thread, donation_elem);
    const struct thread *tb = list_entry (b, struct thread, donation_elem);
    return ta->priority > tb->priority;
}

/* 기존 priority_less(호환 용도) */
bool
priority_less (const struct list_elem *a, const struct list_elem *b,
               void *aux UNUSED)
{
    /* 기존 코드에서 사용되던 이름을 유지하도록 구현 (우선순위 내림차순) */
    return compare_thread_priority_elem(a, b, aux);
}

/* ====================== 우선순위/기부 관련 헬퍼 ====================== */

/* 주어진 큐 레벨에 해당하는 list 반환 (MLFQS용) */
static struct list *mlfqs_get_queue(enum mlfqs_queue level) {
  switch (level) {
    case Q0: return &ready_list_q0;
    case Q1: return &ready_list_q1;
    case Q2: return &ready_list_q2;
    default: NOT_REACHED ();
  }
}

/* T의 effective priority를 original_priority와 donations를 기반으로 재계산 */
void
thread_update_priority (struct thread *t)
{
    int max_priority = t->original_priority;

    if (!list_empty(&t->donations)) {
        struct thread *donor = list_entry(list_front(&t->donations), struct thread, donation_elem);
        if (donor->priority > max_priority)
            max_priority = donor->priority;
    }
    t->priority = max_priority;
}

/* 현재 스레드(cur)가 기다리는 락 체인에 대해 재귀(최대 depth 제한)로 우선순위 기부 */
void
thread_donate_priority (void)
{
    struct thread *cur = thread_current ();
    struct lock *lock;
    for (int depth = 0; depth < 8; depth++) {
        lock = cur->wait_on_lock;
        if (lock == NULL || lock->holder == NULL)
            break;
        struct thread *holder = lock->holder;
        if (holder->priority < cur->priority) {
            holder->priority = cur->priority;
            cur = holder;
        } else {
            break;
        }
    }
}

/* 특정 lock 때문에 생긴 donations만 제거하고 우선순위 재계산 */
void
thread_remove_donation (struct lock *lock)
{
    if (lock == NULL) return;
    struct thread *holder = lock->holder;
    if (holder == NULL) return;

    struct list_elem *e = list_begin(&holder->donations);
    while (e != list_end(&holder->donations)) {
        struct list_elem *next = list_next(e);
        struct thread *donor = list_entry(e, struct thread, donation_elem);

        if (donor->wait_on_lock == lock) {
            list_remove(&donor->donation_elem);
        }

        e = next;
    }

    thread_update_priority(holder);

    /* 우선순위가 낮아졌다면 선점 고려 */
    if (!thread_mlfqs) {
        if (!list_empty(&ready_list)) {
            struct thread *top = list_entry(list_front(&ready_list), struct thread, elem);
            if (top->priority > holder->priority) {
                if (holder == thread_current()) {
                    /* 현재 실행 중인 스레드라면 양보 */
                    if (!intr_context())
                        thread_yield();
                    else
                        intr_yield_on_return();
                }
                else {
                    /* holder가 아닌 경우에는 그냥 리턴 (scheduler가 알아서) */
                }
            }
        }
    }
}

/* ====================== 스레드 시스템 초기화/관리 ====================== */

void
thread_init (void)
{
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);

    if (thread_mlfqs) {
        list_init (&ready_list_q0);
        list_init (&ready_list_q1);
        list_init (&ready_list_q2);
    } else {
        list_init (&ready_list);
    }

    list_init (&all_list);
    list_init (&sleep_list);

    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}

void
thread_start (void)
{
    idle_thread = thread_create ("idle", PRI_MIN, idle, NULL);
    intr_enable ();
}

void
thread_tick (void)
{
    struct thread *cur = thread_current ();

    if (cur == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (cur->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    if (!thread_mlfqs) {
        enum intr_level old_level = intr_disable();
        struct list_elem *e = list_begin(&ready_list);
        while (e != list_end(&ready_list)) {
            struct thread *t = list_entry(e, struct thread, elem);
            t->age++;
            if (t->age >= AGING_TICKS) {
                t->age = 0;
                if (t->priority < PRI_DEFAULT) {
                    /* 우선순위 증가 후 재삽입 */
                    list_remove(&t->elem);
                    t->priority++;
                    list_insert_ordered(&ready_list, &t->elem, compare_thread_priority_elem, NULL);
                    if (t->priority > cur->priority) {
                        intr_yield_on_return();
                    }
                    /* iterator invalidation 방지: restart from head 안전 방법이 필요하나 간단화 */
                }
            }
            e = list_next(e);
        }
        intr_set_level(old_level);
    } else {
        enum intr_level old_level = intr_disable();
        bool promoted_to_q0 = false;

        for (int q = Q2; q >= Q0; q--) {
            struct list *q_list = mlfqs_get_queue((enum mlfqs_queue)q);
            struct list_elem *e = list_begin(q_list);
            while (e != list_end(q_list)) {
                struct thread *t = list_entry(e, struct thread, elem);
                t->age++;
                if (t->age >= AGING_TICKS) {
                    t->age = 0;
                    if (t->mlfqs_queue_level > Q0) {
                        t->mlfqs_queue_level--;
                        e = list_remove(&t->elem);
                        list_push_back(mlfqs_get_queue(t->mlfqs_queue_level), &t->elem);
                        if (t->mlfqs_queue_level == Q0)
                            promoted_to_q0 = true;
                        continue;
                    }
                }
                e = list_next(e);
            }
        }

        if (promoted_to_q0 && cur->mlfqs_queue_level != Q0)
            intr_yield_on_return();

        if (cur != idle_thread) {
            cur->mlfqs_ticks++;
            int current_slice = (cur->mlfqs_queue_level == Q0) ? MLFQS_Q0_SLICE :
                                (cur->mlfqs_queue_level == Q1) ? MLFQS_Q1_SLICE :
                                MLFQS_Q2_SLICE;
            if (cur->mlfqs_ticks >= current_slice) {
                cur->mlfqs_ticks = 0;
                if (cur->mlfqs_queue_level < Q2)
                    cur->mlfqs_queue_level++;
                thread_yield();
            }
        }
        intr_set_level(old_level);
    }
}

void
thread_print_stats (void)
{
    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
            idle_ticks, kernel_ticks, user_ticks);
}

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

    t = palloc_get_page (PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    init_thread (t, name, priority);
    tid = t->tid = allocate_tid ();

    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void)) kernel_thread;

    sf = alloc_frame (t, sizeof *sf);
    sf->eip = (void (*) (void)) switch_entry;
    sf->ebp = 0;

    old_level = intr_disable ();
    thread_unblock (t);
    intr_set_level (old_level);

    if (!thread_mlfqs) {
        if (t->priority > thread_current()->priority)
            thread_yield();
    }

    return tid;
}

void
thread_block (void)
{
    ASSERT (!intr_context ());
    ASSERT (intr_get_level () == INTR_OFF);
    thread_current ()->status = THREAD_BLOCKED;
    schedule ();
}

void
thread_unblock (struct thread *t)
{
    enum intr_level old_level;

    ASSERT (is_thread (t));

    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED || t->status == THREAD_READY);
    if (t->status != THREAD_READY) {
        if (thread_mlfqs) {
            t->mlfqs_ticks = 0;
            list_push_back (mlfqs_get_queue(t->mlfqs_queue_level), &t->elem);
        } else {
            list_insert_ordered (&ready_list, &t->elem, compare_thread_priority_elem, NULL);
        }
        t->status = THREAD_READY;
    }
    intr_set_level (old_level);

    if (!thread_mlfqs) {
        if (t->priority > thread_current()->priority) {
            if (!intr_context())
                thread_yield();
            else
                intr_yield_on_return();
        }
    }
}

struct thread *
thread_current (void)
{
    return running_thread ();
}

tid_t
thread_tid (void)
{
    return thread_current ()->tid;
}

const char *
thread_name (void)
{
    return thread_current ()->name;
}

void
thread_exit (void) 
{
    ASSERT (!intr_context ());

#ifdef USERPROG
    process_exit ();
#endif

    intr_disable ();
    list_remove (&thread_current()->allelem);
    thread_current()->status = THREAD_DYING;
    schedule ();
    NOT_REACHED ();
}

void
thread_yield (void)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (!intr_context ());

    old_level = intr_disable ();
    if (cur != idle_thread) {
        if (thread_mlfqs) {
            list_push_back (mlfqs_get_queue(cur->mlfqs_queue_level), &cur->elem);
        } else {
            list_insert_ordered (&ready_list, &cur->elem, compare_thread_priority_elem, NULL);
        }
    }
    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}

void
thread_foreach (thread_action_func *action, void *aux)
{
    struct list_elem *e;
    for (e = list_begin (&all_list); e != list_end (&all_list);
         e = list_next (e)) {
        struct thread *t = list_entry (e, struct thread, allelem);
        action (t, aux);
    }
}

void
thread_set_priority (int new_priority)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level = intr_disable ();

    cur->original_priority = new_priority;
    thread_update_priority(cur);

    /* 필요한 경우에만 양보하도록 변경 */
    bool should_yield = false;
    if (!thread_mlfqs) {
        if (!list_empty(&ready_list)) {
            struct thread *top = list_entry(list_front(&ready_list), struct thread, elem);
            if (top->priority > cur->priority)
                should_yield = true;
        }
    }
    intr_set_level(old_level);

    if (should_yield) {
        if (!intr_context())
            thread_yield();
        else
            intr_yield_on_return();
    }
}

int
thread_get_priority (void)
{
    return thread_current ()->priority;
}

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
    t->original_priority = priority;
    list_init(&t->donations);
    t->wait_on_lock = NULL;
    t->magic = THREAD_MAGIC;
    list_push_back (&all_list, &t->allelem);

    t->age = 0;
    t->mlfqs_queue_level = Q2;
    t->mlfqs_ticks = 0;
    t->wakeup_tick = 0;
}

struct thread *
running_thread (void)
{
    uint32_t *esp;
    asm ("mov %%esp, %0" : "=g" (esp));
    return (struct thread *) ((unsigned) esp & ~PGMASK);
}

static bool
is_thread (struct thread *t)
{
    return t != NULL && t->magic == THREAD_MAGIC;
}

static void *
alloc_frame (struct thread *t, size_t size)
{
    uint8_t *base = (uint8_t *) t + PGSIZE;
    base -= size;
    base -= sizeof (void *);
    return base;
}

static struct thread *
next_thread_to_run (void)
{
    if (thread_mlfqs) {
        for (enum mlfqs_queue q = Q0; q <= Q2; q++) {
            struct list *q_list = mlfqs_get_queue(q);
            if (!list_empty(q_list))
                return list_entry (list_pop_front (q_list), struct thread, elem);
        }
    } else {
        if (!list_empty (&ready_list))
            return list_entry (list_pop_front (&ready_list), struct thread, elem);
    }
    return idle_thread;
}

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

void
thread_schedule_tail (struct thread *prev)
{
    struct thread *cur = running_thread ();

    ASSERT (intr_get_level () == INTR_OFF);

#ifdef USERPROG
    process_activate ();
#endif

    if (prev != NULL && prev->status == THREAD_DYING) {
        ASSERT (prev != cur);
        palloc_free_page (prev);
    }
}

static void
idle (void *aux UNUSED) 
{
    for (;;) {
        enum intr_level old_level;
        old_level = intr_disable ();
        if (next_thread_to_run () == idle_thread)
            switch_to_idle ();
        intr_set_level (old_level);
    }
}

static void
kernel_thread (thread_func *function, void *aux) 
{
    ASSERT (function != NULL);

    intr_enable ();
    function (aux);
    thread_exit ();
}
