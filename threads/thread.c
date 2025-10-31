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

/* (중략) 기존 전역 변수들 유지 (ready_list, all_list, idle_thread, initial_thread, tid_lock, mlfq[3], thread_mlfqs) */

/* MLFQ Constants */
#define TIME_SLICE 4 // 기존 라운드 로빈 TIME_SLICE
#define TIME_SLICE_Q0 2
#define TIME_SLICE_Q1 4
#define TIME_SLICE_Q2 8
#define AGE_LIMIT 20

/* 함수 선언 (기존 함수 외) */
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

/* 우선순위 비교 및 기부 관련 함수 선언 */
bool thread_cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
bool thread_cmp_lock_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED); // synch.h에 있어야 함
int thread_get_max_ready_priority(void);
void thread_donate_priority(struct thread *t, int priority);
void thread_remove_lock(struct lock *lock);

/* 스케줄링 및 에이징 함수 선언 */
void mlfq_update(void);
void aging_ready_threads(void);


/* thread_init - 기존과 동일. 단, init_thread 호출 시 우선순위 기부 필드 초기화 확인. */
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

/* (중략) thread_start, thread_tick, thread_print_stats - thread_tick에 mlfq/aging 로직 추가 */
void
thread_tick (void)
{
   struct thread *t = thread_current ();
    
    /* 기존: 통계 업데이트 로직 */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    /* [수정] MLFQS/Aging 로직 추가 */
    if (thread_mlfqs) {
        // MLFQS 모드: 
        // 1. 현재 스레드의 recent_cpu 증가 (mlfq_update 내부에서 처리될 수 있음)
        // 2. 큐별 Time Slice 소모 및 강등 처리 (내부에서 yield 유도)
        // 3. 대기 스레드 에이징 및 승급 처리 (내부에서 yield 유도)
        mlfq_update();
    }
    else {
        // Priority Scheduling 모드:
        // 대기 스레드 에이징 (priority++ 및 ready_list 재정렬)
        aging_ready_threads(); 
    }
    
    /* Enforce preemption. */
    // [수정] MLFQS 모드에서는 mlfq_update에서 큐별 time slice를 관리하므로,
    // 일반 Priority Scheduling 모드에서만 기존 TIME_SLICE을 사용하여 라운드 로빈 정책을 유지합니다.
    if (thread_mlfqs == false) {
        if (++thread_ticks >= TIME_SLICE)
            intr_yield_on_return ();
    }
    // MLFQS 모드의 타임 슬라이스(Q0: 2, Q1: 4, Q2: 8)는 mlfq_update() 내에서 관리됩니다.
}
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
        if (t->priority > thread_current()->priority && !intr_context())
            thread_yield();
    }
    else { 
        /* MLFQS Scheduling: Q0으로 초기화하고 push_back */
        if (t->queue_level == -1) {
            t->queue_level = 0;
            t->age[0] = t->age[1] = t->age[2] = 0;
        }
        list_push_back(&mlfq[t->queue_level], &t->elem);
         
        // [추가] MLFQS 선점 로직: 언블록된 스레드가 현재 스레드보다 높은 큐 레벨(낮은 인덱스)에 위치하면 선점
        if (t->queue_level < thread_current()->queue_level) {
            intr_yield_on_return();
        }
    }

    intr_set_level (old_level);
}
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

/* (중략) thread_foreach - 기존과 동일 */

/* thread_set_priority - 우선순위 기부 및 선점 반영 */
/* threads/thread.c */
// (추가 함수) 스레드가 현재 보유한 락을 기준으로 유효 우선순위를 다시 계산하는 함수
void
thread_recalculate_priority(struct thread *t)
{
    // MLFQS 모드일 경우 무시
    if (thread_mlfqs == true) return;
    
    // 원래 우선순위(original_priority)로 시작
    int new_effective_priority = t->original_priority;
    
    // 보유한 모든 락의 최대 기부 우선순위를 찾습니다.
    if (!list_empty(&t->holding_locks)) {
        struct list_elem *e;
        int max_donated_priority = PRI_MIN;
        
        // holding_locks 리스트를 직접 순회하여 최대 max_priority를 찾습니다.
        for (e = list_begin(&t->holding_locks); e != list_end(&t->holding_locks); e = list_next(e)) {
            // lock 구조체는 synch.h에서 정의되었으므로, 해당 lock 구조체에
            // max_priority를 저장하는 필드가 존재해야 합니다.
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


// (메인 함수) 사용자 요청에 의해 호출됨
void
thread_set_priority(int new_priority) 
{
    enum intr_level old_level = intr_disable();
    struct thread *cur = thread_current();
    
    if (thread_mlfqs == false) {
        // 1. 원래 우선순위(original_priority)를 먼저 업데이트
        cur->original_priority = new_priority;
        
        // 2. 새로운 유효 우선순위를 계산하고 cur->priority에 반영 (직접 순회 로직 사용)
        thread_recalculate_priority(cur);

        // 3. 선점 로직 (priority-change 테스트 통과를 위한 핵심)
        // 새로운 우선순위가 ready_list의 최고 우선순위보다 낮아지면 yield (선점)
        if (!list_empty(&ready_list) && cur->priority < thread_get_max_ready_priority())
            thread_yield();
    }
    intr_set_level(old_level);
}

/* (중략) thread_get_priority ~ thread_get_recent_cpu - 기존과 동일 */
/* (중략) idle, kernel_thread - 기존과 동일 */
/* (중략) running_thread, is_thread, alloc_frame - 기존과 동일 */

/* init_thread - 우선순위 기부 필드 초기화 추가 */
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

    /* [추가] Donation 필드 초기화 */
    t->original_priority = priority;
    t->wait_on_lock = NULL;
    list_init (&t->holding_locks);
    
    /* MLFQ / bookkeeping defaults */
    t->queue_level = -1;
    t->recent_cpu = 0;
    t->age[0] = t->age[1] = t->age[2] = 0;
    
    list_push_back (&all_list, &t->allelem);
}


/* next_thread_to_run - priority/mlfqs 분기 */
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

/* (중략) thread_schedule_tail, schedule, allocate_tid - 기존과 동일 */

/* thread_cmp_priority - 우선순위 비교 함수 (높은 우선순위가 앞으로) */
bool
thread_cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
   struct thread *t_a = list_entry(a, struct thread, elem);
   struct thread *t_b = list_entry(b, struct thread, elem);
   return t_a->priority > t_b->priority;
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


/* thread_donate_priority - 우선순위 기부 함수 (재귀적 기부 지원) */
void
thread_donate_priority(struct thread *t, int priority)
{
    if (t->priority < priority) {
        t->priority = priority;
        
        // 연쇄 기부
        if (t->wait_on_lock != NULL && t->wait_on_lock->holder != NULL) {
            if (t->wait_on_lock->max_priority < priority) {
                t->wait_on_lock->max_priority = priority;
            }
            thread_donate_priority(t->wait_on_lock->holder, priority);
        }
        
        // ready 상태라면 리스트에서 제거 후 재삽입하여 정렬 위치 갱신 (선점 유도)
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

    // 참고: 락 해제 로직(holding_locks에서 lock 제거)은 synch.c의 lock_release에서
    // intr_disable() 상태로 처리하는 것이 일반적입니다. 여기서는 재계산만 수행합니다.

    // 새로운 유효 우선순위를 계산하고 cur->priority에 반영 (직접 순회 로직 사용)
    thread_recalculate_priority(cur);
     
    // 우선순위가 낮아진 경우 선점 유도 (priority-sema 테스트를 위한 핵심)
    if (cur->priority < thread_get_max_ready_priority() && !intr_context())
        thread_yield();
    
    intr_set_level(old_level);
}
void
aging_ready_threads(void)
{
    if (thread_mlfqs == false) {
        struct list_elem *e = list_begin (&ready_list);
        while (e != list_end (&ready_list)) {
            struct list_elem *next = list_next(e);
            struct thread *t = list_entry (e, struct thread, elem);

            t->age[0]++; // age[0]을 일반 age로 사용
            
            if (t->age[0] >= AGE_LIMIT) {
                t->age[0] = 0;
                if (t->priority < PRI_DEFAULT) { // PRI_DEFAULT까지만 승급 허용
                    t->priority++;
                    
                    // 우선순위가 바뀌었으므로 리스트에서 제거 후 재삽입
                    list_remove(&t->elem);
                    list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);
                    
                    // 우선순위가 높아졌다면 즉시 선점 여부 확인 (intr_context는 thread_tick에서 이미 off)
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
                t->age[0] = t->age[1] = t->age[2] = 0; 
                list_push_back (&mlfq[t->queue_level], &t->elem);

                // 승급으로 Q0에 스레드가 추가되었고, 현재 스레드보다 우선순위가 높다면 선점
                if (t->queue_level < cur->queue_level && t->queue_level == 0) {
                    intr_yield_on_return();
                }
            }
            e = next;
        }
    }
}
