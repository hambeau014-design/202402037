/* synch.c */

/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE. ... */
void
sema_init (struct semaphore *sema, unsigned value)
{
    ASSERT (sema != NULL);
    sema->value = value;
    list_init (&sema->waiters);
}

/* Down or "P" operation, also known as "wait". ... */
void
sema_down (struct semaphore *sema)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (sema != NULL);
    ASSERT (!intr_context ());

    old_level = intr_disable ();
    if (sema->value > 0) 
        sema->value--;
    else 
        {
            cur->status = THREAD_BLOCKED;
            
            // ===================================================================
            // *** MODIFICATION: list_insert_ordered를 사용하여 우선순위 순으로 삽입 ***
            list_insert_ordered (&sema->waiters, &cur->elem, priority_less, NULL);
            // ===================================================================
            
            thread_block ();
        }
    intr_set_level (old_level);
}

/* Tries to decrement the semaphore's value, without blocking. ... */
bool
sema_try_down (struct semaphore *sema)
{
    enum intr_level old_level;
    bool success;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
    if (sema->value > 0) 
        {
            sema->value--;
            success = true;
        }
    else
        success = false;
    intr_set_level (old_level);

    return success;
}

/* Up or "V" operation, also known as "signal". ... */
void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);
    old_level = intr_disable ();
    if (!list_empty (&sema->waiters)) 
        {
            // ===================================================================
            // *** MODIFICATION: 정렬된 리스트에서 최고 우선순위 스레드를 꺼냅니다. ***
            struct thread *t = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
            thread_unblock (t); 
            // ===================================================================
        }
    sema->value++;
    intr_set_level (old_level);
}

/* Self test for semaphores. ... */
void sema_self_test (void) { /* ... (구현 생략) ... */ }

// ===================================================================
// *** MODIFICATION: Lock 관련 함수 구현 (우선순위 기부 포함) ***

/* Initializes lock LOCK. */
void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL);
    lock->holder = NULL;
    sema_init (&lock->semaphore, 1);
    list_init (&lock->waiters);      /* 대기 리스트 초기화 */
    lock->max_priority = PRI_MIN;    /* 대기 중인 스레드의 최대 우선순위 */
}

/* Acquires LOCK, sleeping until it becomes available if necessary. */
void
lock_acquire (struct lock *lock)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (lock != NULL); /* <- priority-fifo 패닉이 발생했던 라인. 호출부의 문제였으므로, 여기서 lock != NULL을 보장합니다. */
    ASSERT (!intr_context ());
    ASSERT (!lock_held_by_current_thread (lock));

    old_level = intr_disable ();

    if (lock->holder != NULL) {
        // 락 소유자에게 우선순위 기부
        cur->wait_on_lock = lock;
        list_insert_ordered (&lock->waiters, &cur->donation_elem, priority_less, NULL);
        
        // 락 소유자(lock->holder)의 우선순위를 업데이트
        thread_donate_priority();
    }
    
    sema_down (&lock->semaphore);

    // 락 획득 후 기부받은 우선순위를 제거합니다.
    if (lock->holder != NULL) {
        list_remove (&cur->donation_elem);
        thread_remove_donation(lock);
    }

    lock->holder = cur;
    cur->wait_on_lock = NULL;

    intr_set_level (old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure. ... */
bool
lock_try_acquire (struct lock *lock)
{
    enum intr_level old_level;
    bool success;

    ASSERT (lock != NULL);
    ASSERT (!lock_held_by_current_thread (lock));

    old_level = intr_disable ();
    success = sema_try_down (&lock->semaphore);
    if (success) {
        lock->holder = thread_current ();
    }
    intr_set_level (old_level);

    return success;
}

/* Releases LOCK, which must be owned by the current thread. */
void
lock_release (struct lock *lock)
{
    enum intr_level old_level;

    ASSERT (lock != NULL);
    ASSERT (lock_held_by_current_thread (lock));

    old_level = intr_disable ();

    // 락 해제 전, 락 소유자였던 스레드의 기부 우선순위 회수
    thread_remove_donation(lock); 

    lock->holder = NULL;
    sema_up (&lock->semaphore);
    intr_set_level (old_level);

    // 락을 해제했으므로, 더 높은 우선순위의 스레드가 ready_list에 있다면 선점
    thread_yield ();
}

/* Returns true if the current thread holds LOCK, false otherwise. */
bool
lock_held_by_current_thread (const struct lock *lock)
{
    ASSERT (lock != NULL);
    return lock->holder == thread_current ();
}
// ===================================================================

/* Initializes condition variable COND. ... */
void
cond_init (struct condition *cond)
{
    ASSERT (cond != NULL);
    list_init (&cond->waiters);
}

/* Waits on condition variable COND, which must be protected by
   LOCK. ... */
void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);
    
    // ===================================================================
    // *** MODIFICATION: 조건 변수 대기 큐도 우선순위 정렬 ***
    list_insert_ordered (&cond->waiters, &waiter.elem, priority_less, NULL); 
    // ===================================================================
    
    lock_release (lock);
    sema_down (&waiter.semaphore); 
    lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait. ... */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    if (!list_empty (&cond->waiters))
        // ===================================================================
        // *** MODIFICATION: 정렬된 큐에서 최고 우선순위 스레드를 꺼냅니다. ***
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                      ->semaphore);
        // ===================================================================
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK). ... */
void
cond_broadcast (struct condition *cond, struct lock *lock)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    while (!list_empty (&cond->waiters))
        // ===================================================================
        // *** MODIFICATION: 정렬된 큐에서 스레드를 순서대로 모두 꺼냅니다. ***
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                      ->semaphore);
        // ===================================================================
}
