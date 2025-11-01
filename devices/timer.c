#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/thread.h"

/* 8254 PIT */
#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;
static intr_handler_func timer_interrupt;

/* Calibration helpers (Pintos 기본 구현). */
static unsigned loops_per_tick;

static bool too_many_loops (unsigned loops) {
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start) barrier ();
  /* Run loops until another tick arrives. */
  start = ticks;
  while (ticks == start) {
    if (--loops == 0)
      return true;
    barrier ();
  }
  return false;
}

static void busy_wait (int64_t loops) {
  while (loops-- > 0)
    barrier ();
}

static void real_time_sleep (int64_t num, int32_t denom) {
  /* Convert num/denom seconds into timer ticks, and sleep for that many ticks. */
  int64_t t = (int64_t) num * TIMER_FREQ / denom;
  if (t > 0) {
    timer_sleep (t);
  } else {
    /* Busy wait for more precise short intervals. */
    int64_t loops = (int64_t) loops_per_tick * num / denom;
    if (loops <= 0) loops = 1;
    busy_wait (loops);
  }
}

static void real_time_delay (int64_t num, int32_t denom) {
  /* Busy wait for num/denom seconds. */
  int64_t loops = (int64_t) loops_per_tick * num / denom;
  if (loops <= 0) loops = 1;
  busy_wait (loops);
}

/* Public API */
void timer_init (void) {
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

void timer_calibrate (void) {
  unsigned high_bit, test_bit;

  printf ("Calibrating timer...  ");
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) {
    loops_per_tick <<= 1;
    ASSERT (loops_per_tick != 0);
  }

  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != 0; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%u loops/s.\n", loops_per_tick * TIMER_FREQ);
}

int64_t timer_ticks (void) {
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

int64_t timer_elapsed (int64_t then) {
  return timer_ticks () - then;
}

/* Sleep N ticks (non-busy). */
void timer_sleep (int64_t t) {
  if (t <= 0) return;
  int64_t start = timer_ticks ();
  thread_sleep_until (start + t);
}

/* Convenience */
void timer_msleep (int64_t ms) { real_time_sleep (ms, 1000); }
void timer_usleep (int64_t us) { real_time_sleep (us, 1000 * 1000); }
void timer_nsleep (int64_t ns) { real_time_sleep (ns, 1000 * 1000 * 1000); }
void timer_mdelay (int64_t ms) { real_time_delay (ms, 1000); }
void timer_udelay (int64_t us) { real_time_delay (us, 1000 * 1000); }
void timer_ndelay (int64_t ns) { real_time_delay (ns, 1000 * 1000 * 1000); }

/* Interrupt handler: increase tick and call thread_tick() to handle preemption,
   aging, MLFQS demote/promote and timed wakeups. */
static void timer_interrupt (struct intr_frame *args UNUSED) {
  ticks++;
  thread_tick ();
}
