#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <round.h>
#include <stdint.h>

void timer_init (void);
void timer_calibrate (void);

void    timer_sleep (int64_t ticks);
void    timer_msleep (int64_t ms);
void    timer_usleep (int64_t us);
void    timer_nsleep (int64_t ns);
void    timer_mdelay (int64_t ms);
void    timer_udelay (int64_t us);
void    timer_ndelay (int64_t ns);

int64_t timer_ticks (void);
int64_t timer_elapsed (int64_t then);

#endif
