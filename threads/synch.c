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
static bool
sema_thread_priority_cmp (const struct list_elem *a,
                          const struct list_elem *b,
                          void *aux UNUSED)
{
    const struct thread *ta = list_entry (a, struct thread, elem);
    const struct thread *tb = list_entry (b, struct thread, elem);
    return ta->priority > tb->priority;
}

/* semaphore initialization */
void
sema_init (struct semaphore *sema, unsigned value)
{
    ASSERT (sema != NULL);
    sema->value = value;
    list_init (&sema->waiters);
}

/* --- helper compare funcs local to synch.c --- */

/* compare donation list entries (donation_elem) */
static bool
donation_priority_cmp (const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED)
{
    const struct thread *ta = list_entry (a, struct thread, donation_elem);
    const struct thread *tb = list_entry (b, struct thread, donation_elem);
    return ta->priority > tb->priority;
}

/* compare semaphore_elem entries inside condition variable waiters list.
   Compare by the priority of the front thread in each semaphore's waiters. */
static bool
cond_sema_priority_cmp (const struct list_elem *a,
                        const struct list_elem *b,
                        void *aux UNUSED)
{
    const struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
    const struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

    /* If sema waiters empty, treat as lowest priority */
    if (list_empty(&sa->semaphore.waiters) && list_empty(&sb->semaphore.waiters))
        return false;
    if (list_empty(&sa->semaphore.waiters))
        return false;
    if (list_empty(&sb->semaphore.waiters))
        return true;

    const struct thread *ta = list_entry(list_front(&sa->semaphore.waiters), struct thread, elem);
    const struct thread *tb = list_entry(list_front(&sb->semaphore.waiters), struct thread, elem);

    return ta->priority > tb->priority;
}

/* sema_down: waiters inserted by priority (thread.elem) */
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
        list_insert_ordered (&sema->waiters, &cur->elem, sema_thread_priority_cmp, NULL);
        thread_block ();
    }
    intr_set_level (old_level);
}

/* sema_try_down unchanged */
bool
sema_try_down (struct semaphore *sema)
{
    enum intr_level old_level;
    bool success;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
    if (sema->value > 0) {
        sema->value--;
        success = true;
    } else
        success = false;
    intr_set_level (old_level);

    return success;
}

/* sema_up: wake highest priority waiter; after enabling interrupts, yield if needed */
void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);
    old_level = intr_disable ();

    /* --- 이 한 줄이 매우 중요 --- */
    list_sort (&sema->waiters, sema_thread_priority_cmp, NULL);

    if (!list_empty (&sema->waiters))
    {
        struct thread *t = list_entry (list_pop_front (&sema->waiters),
                                       struct thread, elem);
        thread_unblock (t);
    }

    sema->value++;
    intr_set_level (old_level);

    /* 선점 처리 */
    if (!intr_context())
        thread_yield();
    else
        intr_yield_on_return();
}

/* lock implementation with priority donation support */
void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL);
    lock->holder = NULL;
    sema_init (&lock->semaphore, 1);
    list_init (&lock->waiters);      /* optional helper list */
    lock->max_priority = PRI_MIN;
}

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
        cur->wait_on_lock = lock;
        /* insert donor into holder->donations ordered by donation priority */
        list_insert_ordered(&lock->holder->donations, &cur->donation_elem, donation_priority_cmp, NULL);
        /* propagate donation */
        thread_donate_priority();
    }

    sema_down (&lock->semaphore);

    /* successfully acquired lock */
    lock->holder = cur;
    cur->wait_on_lock = NULL;

    intr_set_level (old_level);
}

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

void
lock_release (struct lock *lock)
{
    enum intr_level old_level;

    ASSERT (lock != NULL);
    ASSERT (lock_held_by_current_thread (lock));

    old_level = intr_disable ();

    /* remove donations caused by this lock and update priority */
    thread_remove_donation(lock);

    lock->holder = NULL;
    sema_up (&lock->semaphore);

    intr_set_level (old_level);

    /* After releasing, yield if higher-priority thread exists */
    if (!intr_context()) {
        thread_yield();
    } else {
        intr_yield_on_return();
    }
}

bool
lock_held_by_current_thread (const struct lock *lock)
{
    ASSERT (lock != NULL);
    return lock->holder == thread_current ();
}

/* condition variable implementation using semaphore_elem ordered by highest waiting thread priority */
void
cond_init (struct condition *cond)
{
    ASSERT (cond != NULL);
    list_init (&cond->waiters);
}

void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);

    /* insert by the priority of the thread that will be waiting in this semaphore */
    list_insert_ordered (&cond->waiters, &waiter.elem, cond_sema_priority_cmp, NULL);

    lock_release (lock);
    sema_down (&waiter.semaphore);
    lock_acquire (lock);
}

void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    if (!list_empty (&cond->waiters)) {
        struct semaphore_elem *sema_elem = list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem);
        sema_up (&sema_elem->semaphore);
    }
}

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
