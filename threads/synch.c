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
            /* Insert current thread into sema waiters in priority order.
               Use the thread's standard elem (elem) because sema waiters
               contain thread list elements. */
            list_insert_ordered (&sema->waiters, &cur->elem, compare_thread_priority, NULL);
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
            /* Pop the highest-priority waiting thread and unblock it.
               sema->waiters is kept ordered by thread priority. */
            struct thread *t = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
            thread_unblock (t); /* thread_unblock handles preemption check */
        }
    sema->value++;
    intr_set_level (old_level);
    thread_yield();
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
    list_init (&lock->waiters);      /* optional helper list (not strictly required) */
    lock->max_priority = PRI_MIN;    /* 대기 중인 스레드의 최대 우선순위 */
}

/* Acquires LOCK, sleeping until it becomes available if necessary. */
void
lock_acquire (struct lock *lock)
{
    struct thread *cur = thread_current ();
    enum intr_level old_level;

    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (!lock_held_by_current_thread (lock));

    old_level = intr_disable ();

    if (lock->holder != NULL) {
        /* 1) 현재 스레드가 이 락을 기다리고 있음을 표시 */
        cur->wait_on_lock = lock;

        /* 2) 기부 정보: 현재(donor)를 holder->donations에 우선순위 정렬로 추가.
           donation_elem는 holder의 donations 리스트에서 사용됩니다. */
        list_insert_ordered (&lock->waiters, &cur->donation_elem, compare_thread_priority, NULL);

        /* 3) 기부 전파: 현재 스레드가 기다리는 락의 소유자에게 우선순위를 전파 */
        thread_donate_priority();
    }

    /* Try to acquire semaphore (this may block and put thread into sema->waiters). */
    sema_down (&lock->semaphore);

    /* We acquired the semaphore: become the lock holder. */
    lock->holder = cur;
    /* Clear wait_on_lock for the acquiring thread */
    cur->wait_on_lock = NULL;

    /* If this thread had been in some other holder->donations list as donor (should be the case),
       its donation_elem might still be in that list until the holder removes it on release.
       We do not remove here; the lock release will take care of removing donors associated with that lock. */

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

    /* Remove donations that were caused by threads waiting on this lock,
       and update current thread's effective priority. */
    thread_remove_donation(lock);

    /* Release the lock holder and signal the semaphore to wake up waiters. */
    lock->holder = NULL;
    sema_up (&lock->semaphore);

    intr_set_level (old_level);

    /* After releasing a lock, yield if there's a higher-priority thread ready. */
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
    /* Insert the semaphore_elem into cond->waiters in priority order.
       We compare by the thread that will be waiting on that semaphore.
       To find that thread, we will use the sema->waiters list's front when signaling. 
       But it's simpler to insert by the priority of current thread via waiter.elem wrapper. */
      list_insert_ordered (&cond->waiters, &waiter.elem, compare_thread_priority, NULL);
    //list_insert_ordered (&cond->waiters, &waiter.elem, 
        (list_less_func *) (bool (*)(const struct list_elem *, const struct list_elem *, void *)) priority_less, NULL);

    lock_release (lock);
    sema_down (&waiter.semaphore); /* sema_down orders waiters by priority already */
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

    if (!list_empty (&cond->waiters)) {
        /* Pop the highest-priority waiter (semaphore_elem) and do sema_up on its semaphore.
           Note: cond->waiters list stores semaphore_elem.elem; sema_up will wake the top thread. */
        struct semaphore_elem *sema_elem = list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem);
        sema_up (&sema_elem->semaphore);
    }
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

    while (!list_empty (&cond->waiters)) {
        struct semaphore_elem *sema_elem = list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem);
        sema_up (&sema_elem->semaphore);
    }
}
