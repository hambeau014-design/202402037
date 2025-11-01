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

/* -------- Internal comparators -------- */

/* Compare two waiters (thread.elem) by priority desc, then ready_stamp asc. */
static bool sema_waiter_cmp (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  if (ta->priority != tb->priority) return ta->priority > tb->priority;
  return ta->ready_stamp < tb->ready_stamp;
}

/* Donation list comparator (thread.donation_elem). */
static bool donation_cmp (const struct list_elem *a,
                          const struct list_elem *b,
                          void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, donation_elem);
  const struct thread *tb = list_entry (b, struct thread, donation_elem);
  if (ta->priority != tb->priority) return ta->priority > tb->priority;
  return ta->ready_stamp < tb->ready_stamp;
}

/* For condition waiters: semaphore elem that itself contains a semaphore. */
struct semaphore_elem {
  struct list_elem elem;      /* in cond->waiters */
  struct semaphore semaphore; /* internal semaphore (0/1) */
};

/* Get effective priority of first waiter inside a semaphore. */
static int sema_front_priority (const struct semaphore *sema) {
  if (list_empty (&sema->waiters)) return PRI_MIN;
  const struct thread *t = list_entry (list_front (&sema->waiters), struct thread, elem);
  return t->priority;
}

/* Compare two condvar waiters by the priority of their front sema waiter. */
static bool cond_waiter_cmp (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED)
{
  const struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
  const struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);
  int pa = sema_front_priority (&sa->semaphore);
  int pb = sema_front_priority (&sb->semaphore);
  if (pa != pb) return pa > pb;

  /* Ties: FIFO by earliest ready_stamp among their fronts. */
  if (!list_empty (&sa->semaphore.waiters) && !list_empty (&sb->semaphore.waiters)) {
    const struct thread *ta = list_entry (list_front (&sa->semaphore.waiters), struct thread, elem);
    const struct thread *tb = list_entry (list_front (&sb->semaphore.waiters), struct thread, elem);
    return ta->ready_stamp < tb->ready_stamp;
  }
  return false;
}

/* -------- Semaphore -------- */
void sema_init (struct semaphore *sema, unsigned value)
{
  sema->value = value;
  list_init (&sema->waiters);
}

void sema_down (struct semaphore *sema)
{
  enum intr_level old = intr_disable ();
  while (sema->value == 0) {
    struct thread *cur = thread_current ();
    /* insert ordered by priority, FIFO among equals */
    list_insert_ordered (&sema->waiters, &cur->elem, sema_waiter_cmp, NULL);
    thread_block ();
  }
  sema->value--;
  intr_set_level (old);
}

bool sema_try_down (struct semaphore *sema)
{
  enum intr_level old = intr_disable ();
  bool ok = false;
  if (sema->value > 0) { sema->value--; ok = true; }
  intr_set_level (old);
  return ok;
}

void
sema_up (struct semaphore *sema)
{
  enum intr_level old = intr_disable ();
  if (!list_empty (&sema->waiters)) {
    list_sort (&sema->waiters, sema_waiter_cmp, NULL);
    struct thread *t = list_entry (list_pop_front (&sema->waiters),
                                   struct thread, elem);
    thread_unblock (t);
  }
  sema->value++;
  intr_set_level (old);

  /* 즉시 선점 */
  intr_yield_on_return ();
}

/* -------- Lock -------- */
void lock_init (struct lock *lock)
{
  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

void lock_acquire (struct lock *lock)
{
  struct thread *cur = thread_current ();
  if (lock->holder != NULL) {
    cur->wait_on_lock = lock;
    list_insert_ordered (&lock->holder->donations, &cur->donation_elem, donation_cmp, NULL);
    thread_donate_priority ();
  }
  sema_down (&lock->semaphore);
  lock->holder = cur;
  cur->wait_on_lock = NULL;
}

bool lock_try_acquire (struct lock *lock)
{
  if (sema_try_down (&lock->semaphore)) { lock->holder = thread_current (); return true; }
  return false;
}

void lock_release (struct lock *lock)
{
  thread_remove_donation (lock);
  lock->holder = NULL;
  sema_up (&lock->semaphore);
}

bool lock_held_by_current_thread (const struct lock *lock)
{
  return lock->holder == thread_current ();
}

/* -------- Condition Variable -------- */
void cond_init (struct condition *cond)
{
  list_init (&cond->waiters);
}

void cond_wait (struct condition *cond, struct lock *lock)
{
  struct semaphore_elem waiter;
  sema_init (&waiter.semaphore, 0);

  list_insert_ordered (&cond->waiters, &waiter.elem, cond_waiter_cmp, NULL);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

void cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
  if (!list_empty (&cond->waiters)) {
    list_sort (&cond->waiters, cond_waiter_cmp, NULL);
    struct semaphore_elem *se =
      list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem);
    sema_up (&se->semaphore);
  }

  if (!intr_context ()) thread_yield ();
  else                  intr_yield_on_return ();
}

void cond_broadcast (struct condition *cond, struct lock *lock)
{
  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}

