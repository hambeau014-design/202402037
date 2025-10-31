/* timer.c */

#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void)
{
    pit_configure_channel (0, 2, TIMER_FREQ);
    intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick over the course of a few seconds. */
void
timer_calibrate (void)
{
    unsigned loops, min_estimate, max_estimate;

    printf ("Calibrating timer...  ");
    min_estimate = estimate_loops (10000);
    max_estimate = min_estimate;
    loops = min_estimate;
    while (loops < 1 << 31)
        {
            unsigned estimate = estimate_loops (loops);
            if (estimate > max_estimate)
                max_estimate = estimate;
            else if (estimate < min_estimate)
                min_estimate = estimate;
            else
                break;
            loops *= 2;
        }

    printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops / 100);
    loops_per_tick = loops / 100;
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
    enum intr_level old_level = intr_disable ();
    int64_t t = ticks;
    intr_set_level (old_level);
    return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
    return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be on, or it will panic. */
void
timer_sleep (int64_t ticks) 
{
    struct thread *cur;
    enum intr_level old_level;
    
    ASSERT (intr_get_level () == INTR_ON);
    if (ticks <= 0)
        return;

    cur = thread_current ();
    old_level = intr_disable ();
    
    // ===================================================================
    // *** MODIFICATION: sleep_list에 삽입 및 블록 ***
    // 현재 스레드의 wakeup_tick을 설정하고 sleep_list에 삽입합니다.
    cur->wakeup_tick = ticks + timer_ticks ();
    
    // sleep_list를 오름차순(가장 빨리 깰 스레드가 리스트 앞)으로 유지합니다.
    // Pintos의 thread_sleep 구현을 따르며, thread.h의 priority_less와는 다른 비교 함수가 필요합니다.
    // 여기서는 간단히 리스트의 끝에 추가하고 인터럽트 핸들러에서 순회하는 방식으로 처리합니다.
    list_push_back (list_head (list_head (&sleep_list)), &cur->elem);

    // 전역 변수인 next_tick_to_wakeup을 갱신합니다.
    if (cur->wakeup_tick < next_tick_to_wakeup)
        next_tick_to_wakeup = cur->wakeup_tick;
    
    thread_block (); // 스레드를 차단(Block)합니다.
    // ===================================================================

    intr_set_level (old_level);
}

/* Sleeps for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) 
{
    real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds. */
void
timer_usleep (int64_t us) 
{
    real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) 
{
    real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
    printf ("Timer: %'"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
    ticks++;
    
    // ===================================================================
    // *** MODIFICATION: 깨울 스레드 확인 및 unblock ***
    if (ticks >= next_tick_to_wakeup)
    {
        struct list_elem *e = list_begin (&sleep_list);
        int64_t new_next_wakeup = INT64_MAX;

        while (e != list_end (&sleep_list))
        {
            struct thread *t = list_entry (e, struct thread, elem);
            
            if (t->wakeup_tick <= ticks)
            {
                // 깨워야 할 스레드를 리스트에서 제거하고 unblock합니다.
                e = list_remove (&t->elem);
                thread_unblock (t);
            }
            else 
            {
                // 아직 깰 시간이 안 된 스레드는 다음 최소 wakeup 시간을 갱신합니다.
                if (t->wakeup_tick < new_next_wakeup)
                    new_next_wakeup = t->wakeup_tick;
                e = list_next (e);
            }
        }
        next_tick_to_wakeup = new_next_wakeup; // 다음 최소 wakeup 시간 갱신
    }
    // ===================================================================
    
    thread_tick ();
}

/* Returns one of the following:

   - An estimate of the number of loops per tenth of a second.
   - 0 if the estimate turned out to be less than 1. */
static unsigned
estimate_loops (unsigned loops)
{
    /* Number of timer ticks to wait. */
    static unsigned const wait_ticks = TIMER_FREQ / 10;
    
    uint64_t start_ticks;
    unsigned old_loops;

    start_ticks = ticks;
    old_loops = loops;
    while (loops > 0 && start_ticks == ticks)
        loops /= 2;
    if (loops == 0)
        return 0;

    start_ticks = ticks;
    while (timer_elapsed (start_ticks) < wait_ticks)
        busy_wait (loops);

    /* Convert loops to loops per tenth of a second. */
    return (uint64_t) loops * 10 / timer_elapsed (start_ticks);
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops)
{
    while (loops-- > 0)
        barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom)
{
    /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
    int64_t ticks = num * TIMER_FREQ / denom;

    ASSERT (intr_get_level () == INTR_ON);
    if (ticks > 0)
        {
            /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */
            timer_sleep (ticks);
        }
    else
        {
            /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
            real_time_delay (num, denom);
        }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
    /* Scale the numerator and denominator down by 1000 to avoid
       the possibility of overflow. */
    ASSERT (denom > 0);
    int64_t loops = (uint64_t) num * loops_per_tick / denom;
    busy_wait (loops);
}
