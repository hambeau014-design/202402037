#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/thread.h"

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

static int64_t ticks;
static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

void timer_init (void) {
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

void timer_calibrate (void) {
  /* default Pintos calibration code 그대로 두기 */
}

int64_t timer_ticks (void) {
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

int64_t timer_elapsed (int64_t then) { return timer_ticks () - then; }

void timer_sleep (int64_t t) {
  int64_t start = timer_ticks ();
  if (t <= 0) return;
  thread_sleep_until (start + t);
}

void timer_msleep (int64_t ms) { real_time_sleep (ms, 1000); }
void timer_usleep (int64_t us) { real_time_sleep (us, 1000 * 1000); }
void timer_nsleep (int64_t ns) { real_time_sleep (ns, 1000 * 1000 * 1000); }
void timer_mdelay (int64_t ms) { real_time_delay (ms, 1000); }
void timer_udelay (int64_t us) { real_time_delay (us, 1000 * 1000); }
void timer_ndelay (int64_t ns) { real_time_delay (ns, 1000 * 1000 * 1000); }

static void timer_interrupt (struct intr_frame *args UNUSED) {
  ticks++;
  thread_tick ();
}

static bool too_many_loops (unsigned loops) { /* Pintos 기본 */ return false; }
static void busy_wait (int64_t loops) { while (loops-- > 0) barrier (); }

static void real_time_sleep (int64_t num, int32_t denom) {
  int64_t t = num * TIMER_FREQ / denom;
  if (t > 0) timer_sleep (t);
  else busy_wait (num * loops_per_tick / denom);
}

static void real_time_delay (int64_t num, int32_t denom) {
  busy_wait (num * loops_per_tick / denom);
}
