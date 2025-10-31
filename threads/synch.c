/* This file is derived from source code for the Nachos (중략) */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE. (중략) */

/* Down or "P" operation on a semaphore. */
void
sema_down (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);
    ASSERT (!intr_context ());

    old_level = intr_disable ();
    while (sema->value == 0)
        {
            // 🚨 [수정] list_push_back 대신 우선순위 순으로 삽입 (Priority Scheduling 필수)
            list_insert_ordered (&sema->waiters, &thread_current ()->elem, thread_cmp_priority, NULL);
            thread_block ();
        }
    sema->value--;
    intr_set_level (old_level);
}

/* Tries to acquire a semaphore via a "down" operation. (중략) */

/* Up or "V" operation on a semaphore. */
void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
    if (!list_empty (&sema->waiters))
        // sema_down에서 우선순위 순으로 삽입했으므로, list_pop_front가 가장 높은 우선순위 스레드를 꺼냄
        thread_unblock (list_entry (list_pop_front (&sema->waiters),
                                    struct thread, elem));
    sema->value++;
    intr_set_level (old_level);
}

/* Initializes lock LOCK. */
void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL);

    lock->holder = NULL;
    sema_init (&lock->semaphore, 1);
    // 🚨 [추가] Donation 필드 초기화
    lock->max_priority = PRI_MIN;
    list_init (&lock->elem); 
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary. */
void
lock_acquire (struct lock *lock)
{
    enum intr_level old_level;
    struct thread *cur = thread_current();

    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (!lock_held_by_current_thread (lock));

    old_level = intr_disable();

    // 1. Priority Donation: 락 보유자에게 우선순위 기부
    if (lock->holder != NULL) {
        // 현재 스레드가 대기해야 하므로, 락과 락 보유자에게 자신의 우선순위를 기부
        cur->wait_on_lock = lock;
        if (cur->priority > lock->max_priority) {
            lock->max_priority = cur->priority;
        }
        // 연쇄 기부 로직은 thread_donate_priority 내부에서 처리
        thread_donate_priority(lock->holder, cur->priority);
    }

    // 2. 세마포어 다운 (블록)
    sema_down (&lock->semaphore);

    // 3. 락 획득 후 기부 정보 리셋 및 보유 정보 업데이트
    cur->wait_on_lock = NULL; // 락을 획득했으므로 대기 중인 락 리셋

    // 4. 락 보유 정보 및 holding_locks 업데이트
    lock->holder = cur;
    list_push_back(&cur->holding_locks, &lock->elem); 

    intr_set_level (old_level);
}

/* Tries to acquire LOCK and returns true if successful. (중략) */

/* Releases LOCK. */
void
lock_release (struct lock *lock)
{
    enum intr_level old_level;
    struct thread *cur = thread_current();

    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    old_level = intr_disable();

    // 1. holding_locks에서 락 제거
    list_remove(&lock->elem);

    // 2. 락 보유자 정보 초기화 및 max_priority 리셋
    lock->holder = NULL;
    // lock->max_priority는 lock_acquire에서 대기하는 스레드가 설정하므로, 
    // holder가 없을 때 PRI_MIN으로 리셋하는 것이 적절
    lock->max_priority = PRI_MIN; 

    // 3. 락 해제 후 현재 스레드의 우선순위 재계산 및 선점 확인
    thread_remove_lock(lock); 
    
    // 4. 세마포어 업 (대기 스레드 깨우기)
    sema_up (&lock->semaphore);

    intr_set_level (old_level);
}

/* Returns true if the current thread holds LOCK, false otherwise. (중략) */

/* Initializes condition variable COND. */
void
cond_init (struct condition *cond)
{
    list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled. (중략) */
void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;
    struct thread *cur = thread_current();

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);
    list_push_back (&cond->waiters, &waiter.elem);
    
    // Cond Wait 동안은 락을 기다리는 상태가 아니므로 wait_on_lock을 초기화
    cur->wait_on_lock = NULL;

    // 🚨 [수정] lock_release에 락 해제 및 우선순위 회수 로직을 위임.
    lock_release (lock); 

    sema_down (&waiter.semaphore);

    // 다시 lock을 획득
    lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then (중략) */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    if (!list_empty (&cond->waiters))
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                      ->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by (중략) */
void
cond_broadcast (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    while (!list_empty (&cond->waiters))
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                      ->semaphore);
}
