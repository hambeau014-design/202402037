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

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void)
{
    unsigned high_bit, test_bit;

    ASSERT (intr_get_level () == INTR_ON);
    printf ("Calibrating timer...  ");

    /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
    loops_per_tick = 1u << 10;
    while (!too_many_loops (loops_per_tick << 1))
        {
            loops_per_tick <<= 1;
            ASSERT (loops_per_tick != 0);
        }

    /* Refine the next 8 bits of loops_per_tick. */
    high_bit = loops_per_tick;
    for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
        if (!too_many_loops (high_bit | test_bit))
            loops_per_tick |= test_bit;

    printf ("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
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
   be turned on. */
void
timer_sleep (int64_t ticks)
{
    struct thread *cur = thread_current();
    enum intr_level old_level;

    ASSERT (intr_get_level () == INTR_ON);

    if (ticks <= 0)
        return;

    old_level = intr_disable ();

    // 1. 깨어날 시간 계산 및 저장
    cur->wake_up_tick = ticks + timer_ticks();

    // 2. Sleep Queue에 삽입 (깨어날 시간 순으로 정렬하여 삽입)
    // Pintos의 sleep_list는 일반적으로 'struct thread'의 'elem'을 사용하며,
    // 정렬된 삽입이 일반적입니다.
    list_insert_ordered (&sleep_list, &cur->elem, thread_cmp_wake_up_tick, NULL);

    // 3. 현재 스레드를 BLOCKED 상태로 전환
    thread_block ();

    intr_set_level (old_level);
}

// [추가 필요] thread.h 또는 timer.h에 thread_cmp_wake_up_tick 함수 선언
// bool thread_cmp_wake_up_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms)
{
    real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us)
{
    real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns)
{
    real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms)
{
    real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us)
{
    real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns)
{
    real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void)
{
    printf ("Timer: %" PRId64 " ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
    struct list_elem *e;
    struct thread *t;

    ticks++;
    
    // [추가] Sleep Queue 확인 및 깨우기 로직
    // sleep_list를 순회하며 깨울 시간이 된 스레드(wake_up_tick <= ticks)를 unblock
    e = list_begin (&sleep_list);
    while (e != list_end (&sleep_list)) {
        t = list_entry (e, struct thread, elem);
        if (t->wake_up_tick <= ticks) {
            e = list_next(e); // list_remove 전에 next 엘리먼트 미리 저장
            list_remove(&t->elem);
            thread_unblock (t);
        } else {
            // 리스트가 wake_up_tick 순으로 정렬되어 있다면, 
            // 현재 스레드가 깰 시간이 안 됐다면 나머지 스레드도 검사할 필요 없음.
            // 하지만 정렬이 안 되어 있을 수도 있으므로, 일단은 순회 유지.
            e = list_next(e); 
        }
    }
    
    thread_tick ();
    // [수정] mlfq_update() 제거. thread_tick() 내부에서 thread_mlfqs에 따라 처리됨.
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops)
{
    /* Wait for a timer tick. */
    int64_t start = ticks;
    while (ticks == start)
        barrier ();

    /* Run LOOPS loops. */
    start = ticks;
    busy_wait (loops);

    /* If the tick count changed, we iterated too long. */
    barrier ();
    return start != ticks;
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
    ASSERT (denom % 1000 == 0);
    busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
}
