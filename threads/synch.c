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

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:
   - down or P() decrements the value, blocking if the value is
     already zero.
   - up or V() increments the value, and wakes up one thread
     waiting in down if any exist. */
void
sema_init (struct semaphore *sema, unsigned value)
{
    ASSERT (sema != NULL);
    sema->value = value;
    list_init (&sema->waiters);
}

/* Down or "P" operation, also known as "wait".

   Attempts to decrement the semaphore's value.  If the value
   is 0, waits until it is greater than 0.  This operation is
   atomic, so the check and decrement act as a single unit.
   When the semaphore is successfully decremented, the thread
   proceeds. */
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

/* Tries to decrement the semaphore's value, without blocking.  If
   the value is 0, returns false.  Otherwise, returns true and
   decrements the value.

   This operation is atomic, so the check and decrement act as a
   single unit. */
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

/* Up or "V" operation, also known as "signal".

   Increments the semaphore's value and wakes up one thread
   waiting in down, if any exist.  This operation is atomic, so
   the increment and wake-up act as a single unit. */
void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);
    old_level = intr_disable ();
    if (!list_empty (&sema->waiters)) 
        {
            // list_pop_front는 정렬된 리스트에서 최고 우선순위 스레드를 꺼낸다.
            struct thread *t = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
            thread_unblock (t); // thread_unblock에서 선점 체크를 수행한다.
        }
    sema->value++;
    intr_set_level (old_level);
}

/* ... (sema_self_test, lock_init, lock_acquire, lock_try_acquire, lock_release, lock_held_by_current_thread, cond_init) ... */

/* Waits on condition variable COND, which must be protected by
   LOCK. The current thread is blocked until another thread calls
   cond_signal() or cond_broadcast() on the same condition
   variable.

   The lock is released before the thread blocks and reacquired
   before it is unblocked. */
void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);
    // 조건 변수 대기열은 FIFO를 유지 (핵심 우선순위 정렬은 sema_down에서 처리)
    list_push_back (&cond->waiters, &waiter.elem); 
    
    lock_release (lock);
    sema_down (&waiter.semaphore); // sema_down에서 우선순위 정렬이 적용됨
    lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    if (!list_empty (&cond->waiters))
        // FIFO 순으로 대기 중인 세마포어 엘리먼트를 꺼낸다.
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                      ->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock)
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
