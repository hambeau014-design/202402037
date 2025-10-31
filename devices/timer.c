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

/* thread.c에 정의된 sleep_list 및 next_tick_to_wakeup 외부 선언 (Pintos 구조 상 필요) */
extern struct list sleep_list;
extern int64_t next_tick_to_wakeup; 

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);
static unsigned estimate_loops (unsigned loops);

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

// ===================================================================
// *** MODIFICATION: 비교 함수 추가 (wakeup_tick 기준) ***
/* 두 리스트 요소(스레드)의 wakeup_tick을 비교합니다. a가 b보다 작으면(먼저 깰 예정이면) true 반환 */
static bool 
wakeup_tick_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct thread *t1 = list_entry (a, struct thread, elem);
  struct thread *t2 = list_entry (b, struct thread, elem);
  return t1->wakeup_tick < t2->wakeup_tick;
}
// ===================================================================


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
    // *** MODIFICATION: sleep_list에 삽입 및 블록 (우선순위 스케줄링) ***
    
    // 현재 스레드의 wakeup_tick을 설정합니다.
    cur->wakeup_tick = ticks + timer_ticks ();
    
    // sleep_list를 wakeup_tick 오름차순으로 삽입합니다. 
    // (가장 먼저 깰 스레드가 리스트의 맨 앞에 위치)
    list_insert_ordered (&sleep_list, &cur->elem, wakeup_tick_less, NULL);

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
    // *** MODIFICATION: 깨울 스레드 확인 및 unblock (우선순위 스케줄링) ***
    if (ticks >= next_tick_to_wakeup)
    {
        // sleep_list는 wakeup_tick 오름차순으로 정렬되어 있으므로, 
        // 맨 앞의 스레드만 확인하면 됩니다.
        struct list_elem *e = list_begin (&sleep_list);
        int64_t new_next_wakeup = INT64_MAX;
        
        // 깰 시간이 된 모든 스레드를 처리합니다.
        while (e != list_end (&sleep_list)) 
        {
            struct thread *t = list_entry (e, struct thread, elem);
            
            if (t->wakeup_tick <= ticks)
            {
                // 깨워야 할 스레드를 리스트에서 제거하고 unblock합니다.
                e = list_remove (e);
                thread_unblock (t);
            }
            else 
            {
                // 맨 앞 스레드의 시간이 아직 안 되었다면, 그 스레드가 다음 최소 wakeup 시간입니다.
                new_next_wakeup = t->wakeup_tick;
                break;
            }
        }
        next_tick_to_wakeup = new_next_wakeup; // 다음 최소 wakeup 시간 갱신
    }
    // ===================================================================
    
    thread_tick ();
}

/* Busy waits for approximately LOOPS iterations. */
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
         the possibility of overflow in the multiplication. */
    ASSERT (denom > 999);
    real_time_delay_internal (num / 1000, denom / 1000);
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   off. */
void
timer_mdelay (int64_t ms) 
{
    real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   off. */
void
timer_udelay (int64_t us) 
{
    real_time_delay (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   off. */
void
timer_ndelay (int64_t ns) 
{
    real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Estimates loops_per_tick by running the timer once for a certain
   number of timer ticks.

   The first time this is called, a low number of loops is passed in
   as the parameter.  Then, based on how many timer ticks actually
   elapsed, a more accurate estimate can be made.

   For subsequent calls, a more accurate estimate is passed in as
   the parameter, and the process is repeated.

   Finally, the most accurate estimate is used to set the global
   loops_per_tick. */
static unsigned
estimate_loops (unsigned loops)
{
    static int started = 0;
    int64_t start_ticks, elapsed_ticks;
    unsigned old_loops;
    
    /* Disable interrupts during the busy wait. */
    enum intr_level old_level = intr_disable ();

    /* If this is the first time, we haven't started counting yet.
       Set a sentinel value, which tells us to begin the timer. */
    if (!started) 
        {
            old_loops = 10000;
            started = 1;
        } 
    else 
        {
            /* Otherwise, use the estimate passed in as the parameter. */
            old_loops = loops;
        }

    /* Record the number of ticks. */
    start_ticks = timer_ticks ();

    /* Loop until the timer ticks exactly one tick past the start
       time. */
    while (timer_ticks () == start_ticks)
        ;

    /* Busy wait for a fixed number of loops. */
    busy_wait (old_loops);
    
    /* Calculate the number of ticks that elapsed. */
    elapsed_ticks = timer_ticks () - start_ticks;
    
    /* Restore interrupts. */
    intr_set_level (old_level);

    /* Return the average number of loops per tick. */
    return old_loops / elapsed_ticks;
}
