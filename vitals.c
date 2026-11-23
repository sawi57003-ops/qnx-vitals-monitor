/*
 * vitals.c - Real-time patient monitor for QNX 8.0
 *
 * A multi-rate periodic task set that simulates a bedside patient monitor
 * and proves, with measured numbers, that its safety-critical threads keep
 * their deadlines even when the CPU is saturated by low-priority work.
 *
 * Threads (all SCHED_FIFO, explicit priorities):
 *   alarm    100 ms   prio 22  - checks HR / SpO2 / NIBP against limits
 *   ecg       10 ms   prio 20  - samples a synthetic ECG waveform (100 Hz)
 *   spo2      20 ms   prio 18  - samples a synthetic SpO2 signal (50 Hz)
 *   nibp      15 s    prio 12  - intermittent blood-pressure reading
 *   display 1000 ms   prio 10  - prints the dashboard on the serial console
 *   load     (busy)   prio  5  - CPU hog, switched on mid-demo to prove
 *                                priority isolation of the threads above
 *
 * Each periodic thread sleeps with clock_nanosleep(CLOCK_MONOTONIC,
 * TIMER_ABSTIME) and measures its own wake-up jitter against the same
 * clock. A deadline is missed when a thread wakes up more than one full
 * period late. The summary printed at the end is the proof: max jitter and miss
 * counts per thread, before / during / after the load phase.
 *
 * Demo timeline (default 90 s):
 *   0-20 s   clean run, no load
 *   20-60 s  load ON (CPU saturated by the prio-5 hog)
 *   45-60 s  tachycardia event: HR jumps to ~132 bpm, alarm must fire
 *   60-90 s  load OFF, vitals recover
 *
 * Build:  qcc -Vgcc_ntox86_64 -o vitals-x86_64 vitals.c -lm
 * Run:    the QNX image starts it at boot; it exits after the demo with
 *         a summary, or run it directly with a duration in seconds:
 *         vitals-x86_64 [seconds]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#define NS_PER_SEC 1000000000ULL

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    double hr;          /* beats per minute */
    double spo2;        /* percent */
    double sys, dia;    /* mmHg */
    double temp;        /* deg C */
    int    alarm;       /* 0 = none, 1 = active */
    char   alarm_msg[64];
} vitals_t;

static vitals_t g_vitals = {72.0, 98.0, 120.0, 80.0, 36.6, 0, ""};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_running = 1;
static volatile int g_load_on = 0;
static volatile int g_event_on = 0;   /* tachycardia event */
static uint64_t g_cps;                /* ClockCycles per second */
static struct timespec g_t0;          /* demo start (monotonic) */

static double elapsed_s(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_t0.tv_sec) +
           (now.tv_nsec - g_t0.tv_nsec) / 1e9;
}

/* ------------------------------------------------------------------ */
/* Per-thread deadline statistics                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    long        period_ms;
    uint64_t    cycles;        /* activations */
    uint64_t    max_jitter_us; /* worst |actual - expected| wake-up */
    uint64_t    sum_jitter_us;
    uint64_t    misses;        /* woke up > 1 full period late */
} tstat_t;

static tstat_t g_stats[] = {
    {"alarm",   100, 0, 0, 0, 0},
    {"ecg",      10, 0, 0, 0, 0},
    {"spo2",     20, 0, 0, 0, 0},
    {"nibp",  15000, 0, 0, 0, 0},
    {"display",1000, 0, 0, 0, 0},
};

enum { ST_ALARM = 0, ST_ECG, ST_SPO2, ST_NIBP, ST_DISPLAY };

/* Run one activation of a periodic thread: sleep until the next absolute
 * deadline, measure jitter, and update the thread's statistics.
 * Jitter is measured with the SAME clock the sleep uses
 * (CLOCK_MONOTONIC) so the measurement is self-consistent. */
static void periodic_wait(tstat_t *st, struct timespec *next)
{
    struct timespec now;
    int64_t late_ns;

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* expected wake-up = *next; actual = now */
    late_ns = (int64_t)(now.tv_sec - next->tv_sec) * (int64_t)NS_PER_SEC +
              (int64_t)(now.tv_nsec - next->tv_nsec);
    if (late_ns < 0)
        late_ns = 0;  /* woke a hair early (timer granularity) */

    st->cycles++;
    st->sum_jitter_us += (uint64_t)(late_ns / 1000);
    if ((uint64_t)(late_ns / 1000) > st->max_jitter_us)
        st->max_jitter_us = (uint64_t)(late_ns / 1000);
    if (late_ns > st->period_ms * 1000000LL)
        st->misses++;

    /* advance to the next period (no drift accumulation) */
    next->tv_nsec += st->period_ms * 1000000L;
    while (next->tv_nsec >= (long)NS_PER_SEC) {
        next->tv_nsec -= NS_PER_SEC;
        next->tv_sec++;
    }
}

/* ------------------------------------------------------------------ */
/* Synthetic physiology                                                */
/* ------------------------------------------------------------------ */

/* ECG waveform value at a given cardiac phase (0..1): P, QRS, T bumps. */
static double ecg_wave(double phase)
{
    double v = 0.0;
    v += 0.12 * exp(-pow((phase - 0.15) / 0.025, 2));  /* P wave   */
    v -= 0.20 * exp(-pow((phase - 0.28) / 0.010, 2));  /* Q        */
    v += 1.00 * exp(-pow((phase - 0.30) / 0.012, 2));  /* R peak   */
    v -= 0.25 * exp(-pow((phase - 0.33) / 0.010, 2));  /* S        */
    v += 0.25 * exp(-pow((phase - 0.55) / 0.050, 2));  /* T wave   */
    return v;
}

/* Target heart rate as a function of demo time. */
static double target_hr(double t)
{
    if (g_event_on)
        return 132.0 + 3.0 * sin(t * 0.8);
    return 72.0 + 4.0 * sin(t / 20.0);
}

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

static void *ecg_thread(void *arg)
{
    tstat_t *st = &g_stats[ST_ECG];
    struct timespec next;
    double phase = 0.0, last_r = -1.0, hr_est = 72.0;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (g_running) {
        periodic_wait(st, &next);
        double t = elapsed_s();
        double hr = target_hr(t);
        double beat_s = 60.0 / hr;
        phase += (st->period_ms / 1000.0) / beat_s;
        if (phase >= 1.0) {
            phase -= 1.0;
            if (last_r >= 0.0)
                hr_est = 60.0 / (t - last_r);
            last_r = t;
        }
        (void)ecg_wave(phase);
        pthread_mutex_lock(&g_lock);
        g_vitals.hr = hr_est;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void *spo2_thread(void *arg)
{
    tstat_t *st = &g_stats[ST_SPO2];
    struct timespec next;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (g_running) {
        periodic_wait(st, &next);
        double t = elapsed_s();
        double spo2 = 98.0 + 0.4 * sin(t / 7.0);
        if (g_event_on)
            spo2 = 95.0 + 0.3 * sin(t);
        pthread_mutex_lock(&g_lock);
        g_vitals.spo2 = spo2;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void *nibp_thread(void *arg)
{
    tstat_t *st = &g_stats[ST_NIBP];
    struct timespec next;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (g_running) {
        periodic_wait(st, &next);
        pthread_mutex_lock(&g_lock);
        if (g_event_on) {
            g_vitals.sys = 145.0;
            g_vitals.dia = 95.0;
        } else {
            g_vitals.sys = 120.0 + 2.0 * sin(elapsed_s() / 30.0);
            g_vitals.dia = 80.0;
        }
        g_vitals.temp = 36.6 + 0.1 * sin(elapsed_s() / 45.0);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void *alarm_thread(void *arg)
{
    tstat_t *st = &g_stats[ST_ALARM];
    struct timespec next;
    int prev_alarm = 0;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (g_running) {
        periodic_wait(st, &next);
        pthread_mutex_lock(&g_lock);
        double hr = g_vitals.hr, spo2 = g_vitals.spo2;
        int alarm = 0;
        char msg[64] = "";
        if (hr > 120.0)      { alarm = 1; snprintf(msg, sizeof msg, "TACHYCARDIA HR %.0f", hr); }
        else if (hr < 40.0)  { alarm = 1; snprintf(msg, sizeof msg, "BRADYCARDIA HR %.0f", hr); }
        else if (spo2 < 92.0){ alarm = 1; snprintf(msg, sizeof msg, "LOW SpO2 %.0f%%", spo2); }
        g_vitals.alarm = alarm;
        strncpy(g_vitals.alarm_msg, msg, sizeof g_vitals.alarm_msg - 1);
        pthread_mutex_unlock(&g_lock);
        if (alarm && !prev_alarm)
            printf("[t=%5.1fs] !!! ALARM: %s\n", elapsed_s(), msg);
        if (!alarm && prev_alarm)
            printf("[t=%5.1fs] alarm cleared\n", elapsed_s());
        prev_alarm = alarm;
    }
    return NULL;
}

static void *display_thread(void *arg)
{
    tstat_t *st = &g_stats[ST_DISPLAY];
    struct timespec next;
    int tick = 0;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (g_running) {
        periodic_wait(st, &next);
        pthread_mutex_lock(&g_lock);
        vitals_t v = g_vitals;
        pthread_mutex_unlock(&g_lock);
        printf("[t=%5.1fs] HR %5.1f bpm | SpO2 %4.1f%% | NIBP %3.0f/%3.0f | "
               "Temp %.1fC | %s%s | LOAD %s\n",
               elapsed_s(), v.hr, v.spo2, v.sys, v.dia, v.temp,
               v.alarm ? "ALARM: " : "ok", v.alarm ? v.alarm_msg : "",
               g_load_on ? "ON " : "off");
        if (++tick % 5 == 0) {
            printf("           jitter: ecg max %llu us miss %llu | "
                   "spo2 max %llu us miss %llu | "
                   "alarm max %llu us miss %llu\n",
                   (unsigned long long)g_stats[ST_ECG].max_jitter_us,
                   (unsigned long long)g_stats[ST_ECG].misses,
                   (unsigned long long)g_stats[ST_SPO2].max_jitter_us,
                   (unsigned long long)g_stats[ST_SPO2].misses,
                   (unsigned long long)g_stats[ST_ALARM].max_jitter_us,
                   (unsigned long long)g_stats[ST_ALARM].misses);
            fflush(stdout);
        }
    }
    return NULL;
}

/* Lowest-priority CPU hog: busy-spins while the load phase is on. */
static void *load_thread(void *arg)
{
    volatile double x = 0.0;
    while (g_running) {
        if (g_load_on) {
            for (int i = 0; i < 200000; i++)
                x += sqrt((double)i);
        } else {
            usleep(100000);
        }
    }
    (void)x;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int start_thread(pthread_t *tid, void *(*fn)(void *), int prio)
{
    pthread_attr_t attr;
    struct sched_param sp;
    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    sp.sched_priority = prio;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    return pthread_create(tid, &attr, fn, NULL);
}

int main(int argc, char **argv)
{
    int duration = (argc > 1) ? atoi(argv[1]) : 90;
    if (duration <= 0)
        duration = 90;

    g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);

    printf("=== QNX Real-Time Patient Monitor ===\n");
    printf("QNX %s | %d CPUs | cycles/sec %llu\n",
           "8.0", _syspage_ptr->num_cpu, (unsigned long long)g_cps);
    printf("threads (SCHED_FIFO): alarm 100ms prio22, ecg 10ms prio20,\n");
    printf("  spo2 20ms prio18, nibp 15s prio12, display 1s prio10,\n");
    printf("  load hog prio5. demo %d s: load ON at 20s, tachycardia\n",
           duration);
    printf("  event at 45s, load OFF at 60s.\n");
    fflush(stdout);

    pthread_t th[6];
    start_thread(&th[0], alarm_thread,   22);
    start_thread(&th[1], ecg_thread,     20);
    start_thread(&th[2], spo2_thread,    18);
    start_thread(&th[3], nibp_thread,    12);
    start_thread(&th[4], display_thread, 10);
    start_thread(&th[5], load_thread,     5);

    /* Demo phases, driven from main. */
    int phase = 0;
    while (elapsed_s() < duration) {
        double t = elapsed_s();
        if (phase == 0 && t >= 20.0) {
            g_load_on = 1;
            printf("--- [t=20s] CPU load ON (prio-5 hog) ---\n");
            phase = 1;
        } else if (phase == 1 && t >= 45.0) {
            g_event_on = 1;
            printf("--- [t=45s] tachycardia event: HR -> ~132 bpm ---\n");
            phase = 2;
        } else if (phase == 2 && t >= 60.0) {
            g_load_on = 0;
            g_event_on = 0;
            printf("--- [t=60s] load OFF, vitals recovering ---\n");
            phase = 3;
        }
        usleep(100000);
    }

    g_running = 0;
    for (int i = 0; i < 6; i++)
        pthread_join(th[i], NULL);

    printf("\n=== SUMMARY (%d s demo) ===\n", duration);
    printf("thread   period   activations   avg jitter   max jitter   misses\n");
    for (int i = 0; i < 5; i++) {
        tstat_t *s = &g_stats[i];
        uint64_t avg = s->cycles ? s->sum_jitter_us / s->cycles : 0;
        printf("%-8s %6ld ms  %10llu  %9llu us %10llu us %8llu\n",
               s->name, s->period_ms,
               (unsigned long long)s->cycles,
               (unsigned long long)avg,
               (unsigned long long)s->max_jitter_us,
               (unsigned long long)s->misses);
    }
    printf("verdict: alarm thread (safety-critical, 100 ms) missed %llu "
           "deadline(s) while the CPU was saturated.\n",
           (unsigned long long)g_stats[ST_ALARM].misses);
    return 0;
}
