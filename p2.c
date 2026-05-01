/*
Operating Systems Project part 2
Aavan Vadiwala and Isabel Gilchrest
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <stddef.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <math.h>

// Process struct, holds all data for one process
typedef struct
{
    char id[3]; // e.g. "A0"
    int arrival_time;
    int num_bursts;
    int *cpu_bursts; // length: num_bursts
    int *io_bursts;  // length: num_bursts - 1
    bool is_cpu_bound;
} Process;

/*For each of the n processes, in order A0 through Z9, perform the steps below, with I/O-bound
processes generated first. Note that all generated values are integers, even though we use a floatingpoint pseudo-random number generator.
Define your exponential distribution pseudo-random number generation function as next_exp()
(or another similar name) and have it return the next pseudo-random number in the sequence,
capped by *(argv+5).
*/
void createPIDs(int n, char **processIDs)
{
    char letter = 'A';
    for (int i = 0; i < n; i++)
    {
        processIDs[i] = malloc(4); // Space for 'A0\0'
        sprintf(processIDs[i], "%c%d", letter + (i / 10), i % 10);
    }
}

/*2. Identify the number of CPU bursts for the given process as the “ceiling” of the next random
number generated from the uniform distribution obtained via drand48(), then multiplied
by 16; this should obtain a random integer in the inclusive range [1, 16]*/
double next_exp(double lamda, int upperBound)
{
    double val = upperBound;
    while (val >= upperBound)
    {
        double r = drand48();
        val = -log(r) / lamda;
    }
    return val;
}

void getNumBurstsarrivals(double lamda, int upperBound, int *arrival, int *num_bursts)
{
    *arrival = (int)floor(next_exp(lamda, upperBound));
    *num_bursts = (int)ceil(drand48() * 16); // Use drand48() for uniform dist
}

/*3. For each of CPU burst in turn, identify the CPU burst time and the I/O burst time as
the “ceiling” of the next two pseudo-random numbers given by next_exp(); for I/O-bound
processes, multiply the I/O burst time by 8 such that I/O burst time is close to an order of
magnitude longer than CPU burst time; as noted above, for CPU-bound processes, multiply
the CPU burst time by 6; and for the last CPU burst, do not generate a corresponding I/O
burst time, since each process ends with one final CPU burst*/
void getBursts(int *CPU_burst_times, int *IO_burst_times, double lamda, int upperBound, int num_bursts, bool is_cpu_bound)
{
    for (int i = 0; i < num_bursts - 1; i++)
    {
        CPU_burst_times[i] = (int)ceil(next_exp(lamda, upperBound)) * (is_cpu_bound ? 6 : 1);
        IO_burst_times[i] = (int)ceil(next_exp(lamda, upperBound)) * (is_cpu_bound ? 1 : 8);
    }
    CPU_burst_times[num_bursts - 1] = (int)ceil(next_exp(lamda, upperBound)) * (is_cpu_bound ? 6 : 1);
}

// event type order = tie-break priority (lower = higher priority at same time)
// spec tie-break order: CPU_DONE(0) > CPU_START(1) > IO_DONE(2) > ARRIVAL(3)
// EVT_TIMESLICE has the lowest priority since a slice expiry should be processed
// AFTER any same-time I/O completions or arrivals
#define EVT_CPU_DONE 0
#define EVT_CPU_START 1
#define EVT_IO_DONE 2
#define EVT_ARRIVAL 3
#define EVT_TIMESLICE 4

typedef struct
{
    int time;
    int type;
    int proc_idx;
} Event;

// insert event into sorted array (by time, then type, then proc_idx for ties)
void enqueue_event(Event *q, int *size, Event e)
{
    int i = (*size) - 1;
    while (i >= 0)
    {
        if (q[i].time > e.time)
        {
            q[i + 1] = q[i];
            i--;
            continue;
        }
        if (q[i].time == e.time && q[i].type > e.type)
        {
            q[i + 1] = q[i];
            i--;
            continue;
        }
        if (q[i].time == e.time && q[i].type == e.type && q[i].proc_idx > e.proc_idx)
        {
            q[i + 1] = q[i];
            i--;
            continue;
        }
        break;
    }
    q[i + 1] = e;
    (*size)++;
}

Event dequeue_event(Event *q, int *size)
{
    Event e = q[0];
    for (int i = 0; i < (*size) - 1; i++)
        q[i] = q[i + 1];
    (*size)--;
    return e;
}

void rq_push(int *rq, int *size, int idx) { rq[(*size)++] = idx; }

int rq_pop(int *rq, int *size)
{
    int val = rq[0];
    for (int i = 0; i < (*size) - 1; i++)
        rq[i] = rq[i + 1];
    (*size)--;
    return val;
}

void print_queue(int *rq, int size, Process *procs)
{
    printf("[Q:");
    if (size == 0)
    {
        printf(" -]");
        return;
    }
    for (int i = 0; i < size; i++)
        printf(" %s", procs[rq[i]].id);
    printf("]");
}

typedef struct
{
    double total_wait_cpu, total_wait_io;
    double total_turn_cpu, total_turn_io;
    int bursts_cpu, bursts_io;
    int ctx_switches;
    int ctx_cpu, ctx_io;
    int preemptions;
    int preempt_cpu, preempt_io;
    int cpu_busy_time;
    int sim_end;
    int slice_complete_cpu, slice_complete_io;
} Stats;

void fcfs(Process *procs, int n, int tcs, Stats *out)
{
    // per-process simulation state
    int burst_idx[260] = {0}; // which burst each process is on

    // stats
    int wait_start[260] = {0}; // time process entered ready queue
    double total_wait_cpu = 0, total_wait_io = 0;
    double total_turn_cpu = 0, total_turn_io = 0;
    int bursts_cpu = 0, bursts_io = 0;
    int ctx_switches = 0;
    int ctx_cpu = 0, ctx_io = 0;
    int cpu_busy_time = 0;
    int arrival_for_burst[260] = {0}; // when this burst's "arrival" was
    int sim_end = 0;                  // set when last process terminates

    // event queue and ready queue
    Event events[10000];
    int evt_size = 0;
    int rq[260];
    int rq_size = 0;

    // schedule initial arrivals
    for (int i = 0; i < n; i++)
    {
        Event e = {procs[i].arrival_time, EVT_ARRIVAL, i};
        enqueue_event(events, &evt_size, e);
    }

    int t = 0;
    int cpu_proc = -1;   // process currently running on CPU (-1 = none)
    int cpu_next = -1;   // process being context-switched in (-1 = none)
    int cpu_free_at = 0; // when switch-out finishes; next switch-in can start
    int terminated = 0;

    printf("time 0ms: Simulator started for FCFS [Q: -]\n");

    while (terminated < n)
    {
        Event e = dequeue_event(events, &evt_size);
        t = e.time;
        int pi = e.proc_idx;

        if (e.type == EVT_ARRIVAL)
        {
            // Add to ready queue
            rq_push(rq, &rq_size, pi);
            arrival_for_burst[pi] = t;
            wait_start[pi] = t;

            if (t <= 9999)
            {
                printf("time %dms: Process %s arrived; added to ready queue ", t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }

            // if CPU is completely idle, schedule context switch in for next process
            if (cpu_proc == -1 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int cs_start = t;
                int start_time = cs_start + tcs / 2;
                int waited = cs_start - wait_start[next]; // time spent waiting in queue
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                int burst_len = procs[next].cpu_bursts[burst_idx[next]];
                cpu_busy_time += burst_len;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
        else if (e.type == EVT_CPU_START)
        {
            cpu_proc = pi;
            cpu_next = -1;
            if (t <= 9999)
            {
                printf("time %dms: Process %s started using the CPU for %dms burst ",
                       t, procs[pi].id, procs[pi].cpu_bursts[burst_idx[pi]]);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
        }
        else if (e.type == EVT_CPU_DONE)
        {
            int bi = burst_idx[pi];
            int bursts_left = procs[pi].num_bursts - bi - 1;

            if (bursts_left > 0 && t <= 9999)
            {
                printf("time %dms: Process %s completed a CPU burst; %d burst%s to go ",
                       t, procs[pi].id, bursts_left, bursts_left == 1 ? "" : "s");
                print_queue(rq, rq_size, procs);
                printf("\n");
            }

            cpu_proc = -1;

            if (bursts_left == 0)
            {
                // terminate — no real cs-out needed since nothing's switching in
                // (cpu_free_at NOT updated; it doesn't block the next switch-in)
                terminated++;
                sim_end = t + tcs / 2;
                printf("time %dms: Process %s terminated ", t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            else
            {
                // go to I/O — first half of context switch out blocks next switch-in
                cpu_free_at = t + tcs / 2;
                int io_done = cpu_free_at + procs[pi].io_bursts[bi];
                if (t <= 9999)
                {
                    printf("time %dms: Process %s switching out of CPU; blocking on I/O until time %dms ",
                           t, procs[pi].id, io_done);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                burst_idx[pi]++;
                Event io_done_evt = {io_done, EVT_IO_DONE, pi};
                enqueue_event(events, &evt_size, io_done_evt);
            }

            // turnaround = (cpu_done + tcs/2) - arrival_for_burst
            double turn = (t + tcs / 2) - arrival_for_burst[pi];
            if (procs[pi].is_cpu_bound)
                total_turn_cpu += turn;
            else
                total_turn_io += turn;

            // start next process if ready queue not empty
            // (use t+tcs/2 directly — works whether we terminated or went to I/O,
            //  because this is the earliest the next process's cs-in can start)
            if (rq_size > 0 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int next_cs_can_start = t + tcs / 2;
                int start_time = next_cs_can_start + tcs / 2; // switch-out done + switch-in
                int waited = next_cs_can_start - wait_start[next];
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                int burst_len = procs[next].cpu_bursts[burst_idx[next]];
                cpu_busy_time += burst_len;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
        else if (e.type == EVT_IO_DONE)
        {
            rq_push(rq, &rq_size, pi);
            wait_start[pi] = t;
            arrival_for_burst[pi] = t;

            if (t <= 9999)
            {
                printf("time %dms: Process %s completed I/O; added to ready queue ",
                       t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }

            // if CPU is completely idle, schedule context switch in
            if (cpu_proc == -1 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int cs_start = t;
                int start_time = cs_start + tcs / 2;
                int waited = cs_start - wait_start[next];
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                int burst_len = procs[next].cpu_bursts[burst_idx[next]];
                cpu_busy_time += burst_len;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
    }

    printf("time %dms: Simulator ended for FCFS ", sim_end);
    print_queue(rq, rq_size, procs);
    printf("\n");

    out->total_wait_cpu = total_wait_cpu;
    out->total_wait_io = total_wait_io;
    out->total_turn_cpu = total_turn_cpu;
    out->total_turn_io = total_turn_io;
    out->bursts_cpu = bursts_cpu;
    out->bursts_io = bursts_io;
    out->ctx_switches = ctx_switches;
    out->ctx_cpu = ctx_cpu;
    out->ctx_io = ctx_io;
    out->preemptions = 0;
    out->preempt_cpu = 0;
    out->preempt_io = 0;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
    out->slice_complete_cpu = 0;
    out->slice_complete_io = 0;
}

// insert into sjf ready queue sorted by tau ascending, tie-break by proc_idx
void rq_insert_sjf(int *rq, int *size, int idx, int *tau)
{
    int i = *size - 1;
    while (i >= 0)
    {
        if (tau[rq[i]] > tau[idx] || (tau[rq[i]] == tau[idx] && rq[i] > idx))
        {
            rq[i + 1] = rq[i];
            i--;
        }
        else
            break;
    }
    rq[i + 1] = idx;
    (*size)++;
}

/*
In SJF, processes are stored in the ready queue in order of priority based on their anticipated CPU
burst times. More specifically, the process with the shortest predicted CPU burst time will be
selected as the next process executed by the CPU.
*/

void sjf(Process *procs, int n, int tcs, double lambda, double alpha, Stats *out)
{
    // per-process simulation state
    int burst_idx[260] = {0};
    int wait_start[260] = {0};
    int arrival_for_burst[260] = {0};
    int tau[260];

    // stats
    double total_wait_cpu = 0, total_wait_io = 0;
    double total_turn_cpu = 0, total_turn_io = 0;
    int bursts_cpu = 0, bursts_io = 0;
    int ctx_switches = 0;
    int ctx_cpu = 0, ctx_io = 0;
    int cpu_busy_time = 0;
    int sim_end = 0;

    char *algo_name = (alpha == -1.0) ? "SJF-OPT" : "SJF";

    // initialize tau= ceil(1/lambda) for sjf, actual first burst for opt
    int tau0 = (int)ceil(1.0 / lambda);
    for (int i = 0; i < n; i++)
    {
        tau[i] = (alpha == -1.0) ? procs[i].cpu_bursts[0] : tau0;
        arrival_for_burst[i] = procs[i].arrival_time;
    }

    // event queue and ready queue
    Event events[10000];
    int evt_size = 0;
    int rq[260];
    int rq_size = 0;

    // schedule all initial arrivals
    for (int i = 0; i < n; i++)
    {
        Event e = {procs[i].arrival_time, EVT_ARRIVAL, i};
        enqueue_event(events, &evt_size, e);
    }

    int t = 0;
    int cpu_proc = -1;
    int cpu_next = -1;
    int cpu_free_at = 0;
    int terminated = 0;

    printf("time 0ms: Simulator started for %s [Q: -]\n", algo_name);

    while (terminated < n)
    {
        Event e = dequeue_event(events, &evt_size);
        t = e.time;
        int pi = e.proc_idx;

        if (e.type == EVT_ARRIVAL)
        {
            rq_insert_sjf(rq, &rq_size, pi, tau);
            wait_start[pi] = t;
            if (t <= 9999)
            {
                if (alpha == -1.0)
                {
                    printf("time %dms: Process %s arrived; added to ready queue ", t, procs[pi].id);
                }
                else
                {
                    printf("time %dms: Process %s (tau %dms) arrived; added to ready queue ", t, procs[pi].id, tau[pi]);
                }
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            if (cpu_proc == -1 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int cs_start = t;
                int start_time = cs_start + tcs / 2;
                int waited = cs_start - wait_start[next];
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                int burst_len = procs[next].cpu_bursts[burst_idx[next]];
                cpu_busy_time += burst_len;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
        else if (e.type == EVT_CPU_START)
        {
            cpu_proc = pi;
            cpu_next = -1;
            if (t <= 9999)
            {
                if (alpha == -1.0)
                {
                    printf("time %dms: Process %s started using the CPU for %dms burst ",
                           t, procs[pi].id, procs[pi].cpu_bursts[burst_idx[pi]]);
                }
                else
                {
                    printf("time %dms: Process %s (tau %dms) started using the CPU for %dms burst ",
                           t, procs[pi].id, tau[pi], procs[pi].cpu_bursts[burst_idx[pi]]);
                }
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
        }
        else if (e.type == EVT_CPU_DONE)
        {
            int bi = burst_idx[pi];
            int left = procs[pi].num_bursts - bi - 1;
            cpu_proc = -1;
            // cpu_free_at only set when going to I/O (not on termination)

            if (left > 0 && t <= 9999)
            {
                if (alpha == -1.0)
                {
                    printf("time %dms: Process %s completed a CPU burst; %d burst%s to go ",
                           t, procs[pi].id, left, left == 1 ? "" : "s");
                }
                else
                {
                    printf("time %dms: Process %s (tau %dms) completed a CPU burst; %d burst%s to go ",
                           t, procs[pi].id, tau[pi], left, left == 1 ? "" : "s");
                }
                print_queue(rq, rq_size, procs);
                printf("\n");
            }

            // recalculate tau after burst (sjf only, not opt, and only when not terminating)
            if (alpha != -1.0 && left > 0)
            {
                int actual = procs[pi].cpu_bursts[bi];
                tau[pi] = (int)ceil(alpha * actual + (1.0 - alpha) * tau[pi]);
                if (t <= 9999)
                {
                    printf("time %dms: Recalculated tau for process %s to %dms ",
                           t, procs[pi].id, tau[pi]);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
            }

            // turnaround = (cpu_done + tcs/2) - arrival_for_burst
            double turn = (double)(t + tcs / 2) - arrival_for_burst[pi];
            if (procs[pi].is_cpu_bound)
                total_turn_cpu += turn;
            else
                total_turn_io += turn;

            if (left == 0)
            {
                // terminate
                terminated++;
                sim_end = t + tcs / 2;
                printf("time %dms: Process %s terminated ", t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            else
            {
                // go to I/O — first half of cs-out blocks future I/O completions
                cpu_free_at = t + tcs / 2;
                int io_done = cpu_free_at + procs[pi].io_bursts[bi];
                if (t <= 9999)
                {
                    printf("time %dms: Process %s switching out of CPU; blocking on I/O until time %dms ",
                           t, procs[pi].id, io_done);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                burst_idx[pi]++;
                // for opt mode, update tau to the next actual burst time (used for sorting)
                if (alpha == -1.0)
                    tau[pi] = procs[pi].cpu_bursts[burst_idx[pi]];
                Event io_done_evt = {io_done, EVT_IO_DONE, pi};
                enqueue_event(events, &evt_size, io_done_evt);
            }

            // start next process if ready queue not empty
            // (use t+tcs/2 as the cs-in start time — works for both terminate and I/O)
            if (rq_size > 0 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int next_cs_can_start = t + tcs / 2;
                int start_time = next_cs_can_start + tcs / 2;
                int waited = next_cs_can_start - wait_start[next];
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                int burst_len = procs[next].cpu_bursts[burst_idx[next]];
                cpu_busy_time += burst_len;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
        else if (e.type == EVT_IO_DONE)
        {
            rq_insert_sjf(rq, &rq_size, pi, tau);
            wait_start[pi] = t;
            arrival_for_burst[pi] = t;
            if (t <= 9999)
            {
                if (alpha == -1.0)
                {
                    printf("time %dms: Process %s completed I/O; added to ready queue ", t, procs[pi].id);
                }
                else
                {
                    printf("time %dms: Process %s (tau %dms) completed I/O; added to ready queue ", t, procs[pi].id, tau[pi]);
                }
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            // if CPU is completely idle, schedule context switch in
            if (cpu_proc == -1 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int cs_start = t;
                int start_time = cs_start + tcs / 2;
                int waited = cs_start - wait_start[next];
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                int burst_len = procs[next].cpu_bursts[burst_idx[next]];
                cpu_busy_time += burst_len;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
    }

    printf("time %dms: Simulator ended for %s ", sim_end, algo_name);
    print_queue(rq, rq_size, procs);
    printf("\n");

    out->total_wait_cpu = total_wait_cpu;
    out->total_wait_io = total_wait_io;
    out->total_turn_cpu = total_turn_cpu;
    out->total_turn_io = total_turn_io;
    out->bursts_cpu = bursts_cpu;
    out->bursts_io = bursts_io;
    out->ctx_switches = ctx_switches;
    out->ctx_cpu = ctx_cpu;
    out->ctx_io = ctx_io;
    out->preemptions = 0;
    out->preempt_cpu = 0;
    out->preempt_io = 0;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
    out->slice_complete_cpu = 0;
    out->slice_complete_io = 0;
}

// remove an event from the queue, for cancelling a running process's CPU_DONE when it gets preempted
void cancel_event(Event *q, int *size, int proc_idx, int type)
{
    for (int i = 0; i < *size; i++)
    {
        if (q[i].proc_idx == proc_idx && q[i].type == type)
        {
            for (int j = i; j < *size - 1; j++)
                q[j] = q[j + 1];
            (*size)--;
            return;
        }
    }
}

// sorts by remaining time, not full tau.
// insert into srt ready queue sorted by remaining time, tie-break by proc_idx
void rq_insert_srt(int *rq, int *size, int idx, int *remaining)
{
    int i = *size - 1;
    while (i >= 0)
    {
        if (remaining[rq[i]] > remaining[idx] || (remaining[rq[i]] == remaining[idx] && rq[i] > idx))
        {
            rq[i + 1] = rq[i];
            i--;
        }
        else
            break;
    }
    rq[i + 1] = idx;
    (*size)++;
}

/*
The SRT algorithm is a preemptive version of the SJF algorithm. In SRT, when a process arrives,
as it enters the ready queue, if it has a predicted CPU burst time that is less than the remaining
predicted time of the currently running process, a preemption occurs. When such a preemption
occurs, the currently running process is added back to the ready queue based on its remaining
predicted CPU burst time.
*/
void srt(Process *procs, int n, int tcs, double lambda, double alpha, Stats *out)
{
    // per-process simulation state
    int burst_idx[260] = {0};
    int wait_start[260] = {0};
    int arrival_for_burst[260] = {0};
    int tau[260];
    int remaining[260];

    // stats
    double total_wait_cpu = 0, total_wait_io = 0;
    double total_turn_cpu = 0, total_turn_io = 0;
    int bursts_cpu = 0, bursts_io = 0;
    int ctx_switches = 0;
    int ctx_cpu = 0, ctx_io = 0;
    int cpu_busy_time = 0;
    int sim_end = 0;
    int cpu_run_start = 0;
    int preemptions = 0, preempt_cpu = 0, preempt_io = 0;

    char *algo_name = (alpha == -1.0) ? "SRT-OPT" : "SRT";

    // initialize tau= ceil(1/lambda) for sjf, actual first burst for opt
    int tau0 = (int)ceil(1.0 / lambda);
    int srt_key[260]; // sort key: tau for fresh processes, actual remaining for preempted
    for (int i = 0; i < n; i++)
    {
        tau[i] = (alpha == -1.0) ? procs[i].cpu_bursts[0] : tau0;
        arrival_for_burst[i] = procs[i].arrival_time;
        remaining[i] = procs[i].cpu_bursts[0]; // actual first burst
        srt_key[i] = tau[i];
    }

    // event queue and ready queue
    Event events[10000];
    int evt_size = 0;
    int rq[260];
    int rq_size = 0;

    // schedule all initial arrivals
    for (int i = 0; i < n; i++)
    {
        Event e = {procs[i].arrival_time, EVT_ARRIVAL, i};
        enqueue_event(events, &evt_size, e);
    }

    int t = 0;
    int cpu_proc = -1;
    int cpu_next = -1;
    int cpu_free_at = 0;
    int terminated = 0;

    // Track what stats were credited at last "pick" time (when cpu_next was set).
    // If a swap happens at CPU_START, we reverse these credits and apply for
    // the actually-running process.
    int  last_pick_waited = 0;
    bool last_pick_was_fresh = false;
    bool last_pick_is_cpu_bound = false;

    printf("time 0ms: Simulator started for %s [Q: -]\n", algo_name);

    while (terminated < n)
    {
        Event e = dequeue_event(events, &evt_size);
        t = e.time;
        int pi = e.proc_idx;

        if (e.type == EVT_ARRIVAL)
        {
            wait_start[pi] = t;

            if (cpu_proc != -1)
            {
                // CPU busy — check preemption BEFORE inserting pi
                int time_used = t - cpu_run_start;
                int left = remaining[cpu_proc] - time_used;
                int pred_rem = srt_key[cpu_proc] - time_used;

                if (tau[pi] < pred_rem)
                {
                    // PREEMPT: pi goes straight to CPU, never enters queue
                    if (alpha == -1.0)
                        printf("time %dms: Process %s arrived; preempting %s (remaining time %dms) ", t, procs[pi].id, procs[cpu_proc].id, pred_rem);
                    else
                        printf("time %dms: Process %s (tau %dms) arrived; preempting %s (predicted remaining time %dms) ", t, procs[pi].id, tau[pi], procs[cpu_proc].id, pred_rem);
                    print_queue(rq, rq_size, procs);
                    printf("\n");

                    cpu_busy_time += t - cpu_run_start;
                    cancel_event(events, &evt_size, cpu_proc, EVT_CPU_DONE);

                    remaining[cpu_proc] = left;
                    srt_key[cpu_proc] = pred_rem;
                    wait_start[cpu_proc] = t + tcs / 2;
                    rq_insert_srt(rq, &rq_size, cpu_proc, srt_key);

                    int waited = tcs / 2;
                    srt_key[pi] = tau[pi];
                    if (procs[pi].is_cpu_bound)
                    {
                        total_wait_cpu += waited;
                        bursts_cpu++;
                        ctx_cpu++;
                    }
                    else
                    {
                        total_wait_io += waited;
                        bursts_io++;
                        ctx_io++;
                    }
                    ctx_switches++;
                    preemptions++;
                    if (procs[cpu_proc].is_cpu_bound)
                        preempt_cpu++;
                    else
                        preempt_io++;

                    // remember credit so we can reverse if CPU_START swaps
                    last_pick_waited = waited;
                    last_pick_was_fresh = true;
                    last_pick_is_cpu_bound = procs[pi].is_cpu_bound;

                    int burst_len = remaining[pi];
                    cpu_proc = -1;
                    cpu_next = pi;
                    Event cs = {t + tcs, EVT_CPU_START, pi};
                    enqueue_event(events, &evt_size, cs);
                    Event cd = {t + tcs + burst_len, EVT_CPU_DONE, pi};
                    enqueue_event(events, &evt_size, cd);
                }
                else
                {
                    // no preemption: insert and print
                    srt_key[pi] = tau[pi];
                    rq_insert_srt(rq, &rq_size, pi, srt_key);
                    if (t <= 9999)
                    {
                        if (alpha == -1.0)
                            printf("time %dms: Process %s arrived; added to ready queue ", t, procs[pi].id);
                        else
                            printf("time %dms: Process %s (tau %dms) arrived; added to ready queue ", t, procs[pi].id, tau[pi]);
                        print_queue(rq, rq_size, procs);
                        printf("\n");
                    }
                }
            }
            else
            {
                // CPU idle or switch in progress: insert and print
                srt_key[pi] = tau[pi];
                rq_insert_srt(rq, &rq_size, pi, srt_key);
                if (t <= 9999)
                {
                    if (alpha == -1.0)
                        printf("time %dms: Process %s arrived; added to ready queue ", t, procs[pi].id);
                    else
                        printf("time %dms: Process %s (tau %dms) arrived; added to ready queue ", t, procs[pi].id, tau[pi]);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                if (cpu_next == -1)
                {
                    int next = rq_pop(rq, &rq_size);
                    int cs_start = t;
                    int start_time = cs_start + tcs / 2;
                    int waited = cs_start - wait_start[next];
                    bool is_fresh_srt = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                    if (procs[next].is_cpu_bound)
                    {
                        total_wait_cpu += waited;
                        if (is_fresh_srt) bursts_cpu++;
                        ctx_cpu++;
                    }
                    else
                    {
                        total_wait_io += waited;
                        if (is_fresh_srt) bursts_io++;
                        ctx_io++;
                    }
                    ctx_switches++;
                    last_pick_waited = waited;
                    last_pick_was_fresh = is_fresh_srt;
                    last_pick_is_cpu_bound = procs[next].is_cpu_bound;
                    int burst_len = remaining[next];
                    cpu_next = next;
                    Event cs = {start_time, EVT_CPU_START, next};
                    enqueue_event(events, &evt_size, cs);
                    Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                    enqueue_event(events, &evt_size, cd);
                }
            }
        }
        else if (e.type == EVT_CPU_START)
        {
            // SRT-specific: re-evaluate the candidate (pi == cpu_next) against the
            // current queue head. Between the time we picked cpu_next and now, an
            // I/O completion may have placed a higher-priority process at the queue
            // front. If so, swap: re-insert pi into the queue and pop the new front.
            int actual_pi = pi;
            if (rq_size > 0)
            {
                int head = rq[0];
                int head_key = srt_key[head];
                int pi_key = srt_key[pi];
                bool swap = false;
                if (head_key < pi_key) swap = true;
                else if (head_key == pi_key && head < pi) swap = true;
                if (swap)
                {
                    // pi loses to head — put pi back, pop head
                    rq_insert_srt(rq, &rq_size, pi, srt_key);
                    actual_pi = rq_pop(rq, &rq_size);
                    // cancel the (wrong) pre-scheduled CPU_DONE for pi
                    cancel_event(events, &evt_size, pi, EVT_CPU_DONE);
                    // schedule CPU_DONE for the actual running process
                    Event cd = {t + remaining[actual_pi], EVT_CPU_DONE, actual_pi};
                    enqueue_event(events, &evt_size, cd);

                    // ── reverse credit for pi (it didn't actually run) ─────────
                    if (last_pick_is_cpu_bound)
                    {
                        total_wait_cpu -= last_pick_waited;
                        if (last_pick_was_fresh) bursts_cpu--;
                        ctx_cpu--;
                    }
                    else
                    {
                        total_wait_io -= last_pick_waited;
                        if (last_pick_was_fresh) bursts_io--;
                        ctx_io--;
                    }
                    ctx_switches--;
                    // pi goes back to queue; its wait clock continues from its
                    // original entry — the time it spent as cpu_next was still
                    // queue-waiting time per spec (it never actually got the CPU).
                    // So we leave wait_start[pi] alone.

                    // ── apply credit for actual_pi (it's now actually starting) ─
                    // wait time: from when actual_pi entered queue until cs-in started
                    // cs_start = t - tcs/2 (cs-in took tcs/2)
                    int cs_start_actual = t - tcs / 2;
                    int waited_actual = cs_start_actual - wait_start[actual_pi];
                    if (waited_actual < 0) waited_actual = 0;
                    bool fresh_actual = (remaining[actual_pi] == procs[actual_pi].cpu_bursts[burst_idx[actual_pi]]);
                    if (procs[actual_pi].is_cpu_bound)
                    {
                        total_wait_cpu += waited_actual;
                        if (fresh_actual) bursts_cpu++;
                        ctx_cpu++;
                    }
                    else
                    {
                        total_wait_io += waited_actual;
                        if (fresh_actual) bursts_io++;
                        ctx_io++;
                    }
                    ctx_switches++;
                }
            }
            cpu_proc = actual_pi;
            cpu_next = -1;
            cpu_run_start = t;
            if (t <= 9999)
            {
                int full_burst = procs[actual_pi].cpu_bursts[burst_idx[actual_pi]];
                bool resuming = (remaining[actual_pi] != full_burst);
                if (alpha == -1.0)
                {
                    if (resuming)
                        printf("time %dms: Process %s started using the CPU for remaining %dms of %dms burst ",
                               t, procs[actual_pi].id, remaining[actual_pi], full_burst);
                    else
                        printf("time %dms: Process %s started using the CPU for %dms burst ",
                               t, procs[actual_pi].id, remaining[actual_pi]);
                }
                else
                {
                    if (resuming)
                        printf("time %dms: Process %s (tau %dms) started using the CPU for remaining %dms of %dms burst ",
                               t, procs[actual_pi].id, tau[actual_pi], remaining[actual_pi], full_burst);
                    else
                        printf("time %dms: Process %s (tau %dms) started using the CPU for %dms burst ",
                               t, procs[actual_pi].id, tau[actual_pi], remaining[actual_pi]);
                }
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
        }
        else if (e.type == EVT_CPU_DONE)
        {
            cpu_busy_time += t - cpu_run_start;
            int bi = burst_idx[pi];
            int left = procs[pi].num_bursts - bi - 1;
            cpu_proc = -1;
            // cpu_free_at only set when going to I/O (not on termination)

            if (left > 0 && t <= 9999)
            {
                if (alpha == -1.0)
                {
                    printf("time %dms: Process %s completed a CPU burst; %d burst%s to go ",
                           t, procs[pi].id, left, left == 1 ? "" : "s");
                }
                else
                {
                    printf("time %dms: Process %s (tau %dms) completed a CPU burst; %d burst%s to go ",
                           t, procs[pi].id, tau[pi], left, left == 1 ? "" : "s");
                }
                print_queue(rq, rq_size, procs);
                printf("\n");
            }

            // recalculate tau after burst (only when not terminating)
            if (alpha != -1.0 && left > 0)
            {
                int actual = procs[pi].cpu_bursts[bi];
                tau[pi] = (int)ceil(alpha * actual + (1.0 - alpha) * tau[pi]);
                if (t <= 9999)
                {
                    printf("time %dms: Recalculated tau for process %s to %dms ",
                           t, procs[pi].id, tau[pi]);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
            }

            // turnaround = (cpu_done + tcs/2) - arrival_for_burst
            double turn = (double)(t + tcs / 2) - arrival_for_burst[pi];
            if (procs[pi].is_cpu_bound)
                total_turn_cpu += turn;
            else
                total_turn_io += turn;

            if (left == 0)
            {
                // terminate
                terminated++;
                sim_end = t + tcs / 2;
                printf("time %dms: Process %s terminated ", t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            else
            {
                // go to I/O — first half of cs-out blocks future I/O completions
                cpu_free_at = t + tcs / 2;
                int io_done = cpu_free_at + procs[pi].io_bursts[bi];
                if (t <= 9999)
                {
                    printf("time %dms: Process %s switching out of CPU; blocking on I/O until time %dms ",
                           t, procs[pi].id, io_done);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                burst_idx[pi]++;
                // for opt mode, update tau to the next actual burst time (used for sorting)
                if (alpha == -1.0)
                    tau[pi] = procs[pi].cpu_bursts[burst_idx[pi]];
                Event io_done_evt = {io_done, EVT_IO_DONE, pi};
                enqueue_event(events, &evt_size, io_done_evt);
            }

            // start next process if ready queue not empty
            // (use t+tcs/2 as the cs-in start time — works for both terminate and I/O)
            if (rq_size > 0 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int next_cs_can_start = t + tcs / 2;
                int start_time = next_cs_can_start + tcs / 2;
                int waited = next_cs_can_start - wait_start[next];
                bool is_fresh_srt = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    if (is_fresh_srt) bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    if (is_fresh_srt) bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                last_pick_waited = waited;
                last_pick_was_fresh = is_fresh_srt;
                last_pick_is_cpu_bound = procs[next].is_cpu_bound;
                int burst_len = remaining[next];
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
        else if (e.type == EVT_IO_DONE)
        {
            remaining[pi] = procs[pi].cpu_bursts[burst_idx[pi]];
            srt_key[pi] = tau[pi];
            wait_start[pi] = t;
            arrival_for_burst[pi] = t;

            if (cpu_proc != -1)
            {
                // CPU busy — check preemption BEFORE inserting pi
                int time_used = t - cpu_run_start;
                int left = remaining[cpu_proc] - time_used;
                int pred_rem = srt_key[cpu_proc] - time_used;

                if (tau[pi] < pred_rem)
                {
                    // PREEMPT: pi goes straight to CPU, never enters queue
                    if (alpha == -1.0)
                        printf("time %dms: Process %s completed I/O; preempting %s (remaining time %dms) ", t, procs[pi].id, procs[cpu_proc].id, pred_rem);
                    else
                        printf("time %dms: Process %s (tau %dms) completed I/O; preempting %s (predicted remaining time %dms) ", t, procs[pi].id, tau[pi], procs[cpu_proc].id, pred_rem);
                    print_queue(rq, rq_size, procs);
                    printf("\n");

                    cpu_busy_time += t - cpu_run_start;
                    cancel_event(events, &evt_size, cpu_proc, EVT_CPU_DONE);

                    remaining[cpu_proc] = left;
                    srt_key[cpu_proc] = pred_rem;
                    wait_start[cpu_proc] = t + tcs / 2;
                    rq_insert_srt(rq, &rq_size, cpu_proc, srt_key);

                    int waited = tcs / 2;
                    if (procs[pi].is_cpu_bound)
                    {
                        total_wait_cpu += waited;
                        bursts_cpu++;
                        ctx_cpu++;
                    }
                    else
                    {
                        total_wait_io += waited;
                        bursts_io++;
                        ctx_io++;
                    }
                    ctx_switches++;
                    preemptions++;
                    if (procs[cpu_proc].is_cpu_bound)
                        preempt_cpu++;
                    else
                        preempt_io++;

                    // remember credit so we can reverse if CPU_START swaps
                    last_pick_waited = waited;
                    last_pick_was_fresh = true;
                    last_pick_is_cpu_bound = procs[pi].is_cpu_bound;

                    int burst_len = remaining[pi];
                    cpu_proc = -1;
                    cpu_next = pi;
                    Event cs = {t + tcs, EVT_CPU_START, pi};
                    enqueue_event(events, &evt_size, cs);
                    Event cd = {t + tcs + burst_len, EVT_CPU_DONE, pi};
                    enqueue_event(events, &evt_size, cd);
                }
                else
                {
                    // no preemption: insert and print
                    rq_insert_srt(rq, &rq_size, pi, srt_key);
                    if (t <= 9999)
                    {
                        if (alpha == -1.0)
                            printf("time %dms: Process %s completed I/O; added to ready queue ", t, procs[pi].id);
                        else
                            printf("time %dms: Process %s (tau %dms) completed I/O; added to ready queue ", t, procs[pi].id, tau[pi]);
                        print_queue(rq, rq_size, procs);
                        printf("\n");
                    }
                }
            }
            else
            {
                // CPU idle or switch in progress: insert and print
                rq_insert_srt(rq, &rq_size, pi, srt_key);
                if (t <= 9999)
                {
                    if (alpha == -1.0)
                        printf("time %dms: Process %s completed I/O; added to ready queue ", t, procs[pi].id);
                    else
                        printf("time %dms: Process %s (tau %dms) completed I/O; added to ready queue ", t, procs[pi].id, tau[pi]);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                if (cpu_next == -1)
                {
                    int next = rq_pop(rq, &rq_size);
                    int cs_start = t;
                    int start_time = cs_start + tcs / 2;
                    int waited = cs_start - wait_start[next];
                    bool is_fresh_srt = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                    if (procs[next].is_cpu_bound)
                    {
                        total_wait_cpu += waited;
                        if (is_fresh_srt) bursts_cpu++;
                        ctx_cpu++;
                    }
                    else
                    {
                        total_wait_io += waited;
                        if (is_fresh_srt) bursts_io++;
                        ctx_io++;
                    }
                    ctx_switches++;
                    last_pick_waited = waited;
                    last_pick_was_fresh = is_fresh_srt;
                    last_pick_is_cpu_bound = procs[next].is_cpu_bound;
                    int burst_len = remaining[next];
                    cpu_next = next;
                    Event cs = {start_time, EVT_CPU_START, next};
                    enqueue_event(events, &evt_size, cs);
                    Event cd = {start_time + burst_len, EVT_CPU_DONE, next};
                    enqueue_event(events, &evt_size, cd);
                }
            }
        }
    }

    printf("time %dms: Simulator ended for %s ", sim_end, algo_name);
    print_queue(rq, rq_size, procs);
    printf("\n");

    out->total_wait_cpu = total_wait_cpu;
    out->total_wait_io = total_wait_io;
    out->total_turn_cpu = total_turn_cpu;
    out->total_turn_io = total_turn_io;
    out->bursts_cpu = bursts_cpu;
    out->bursts_io = bursts_io;
    out->ctx_switches = ctx_switches;
    out->ctx_cpu = ctx_cpu;
    out->ctx_io = ctx_io;
    out->preemptions = preemptions;
    out->preempt_cpu = preempt_cpu;
    out->preempt_io = preempt_io;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
    out->slice_complete_cpu = 0;
    out->slice_complete_io = 0;
}

void rr(Process *procs, int n, int tcs, int slice, Stats *out)
{
    // per-process simulation state
    int burst_idx[260] = {0};
    int wait_start[260] = {0};
    int arrival_for_burst[260] = {0};
    int remaining[260];

    // stats
    double total_wait_cpu = 0, total_wait_io = 0;
    double total_turn_cpu = 0, total_turn_io = 0;
    int bursts_cpu = 0, bursts_io = 0;
    int ctx_switches = 0;
    int ctx_cpu = 0, ctx_io = 0;
    int cpu_busy_time = 0;
    int sim_end = 0;
    int cpu_run_start = 0;
    int preemptions = 0, preempt_cpu = 0, preempt_io = 0;
    int slice_complete_cpu = 0, slice_complete_io = 0;

    for (int i = 0; i < n; i++)
    {
        arrival_for_burst[i] = procs[i].arrival_time;
        remaining[i] = procs[i].cpu_bursts[0];
    }

    // event queue and ready queue
    Event events[10000];
    int evt_size = 0;
    int rq[260];
    int rq_size = 0;

    // schedule all initial arrivals
    for (int i = 0; i < n; i++)
    {
        Event e = {procs[i].arrival_time, EVT_ARRIVAL, i};
        enqueue_event(events, &evt_size, e);
    }

    int t = 0;
    int cpu_proc = -1;
    int cpu_next = -1;
    int cpu_free_at = 0;
    int terminated = 0;

    // tracking the cs-out window after a preemption: when a process is preempted
    // by a time slice expiry, it's pushed to the queue tail, but technically only
    // "enters" the queue at last_preempt_t + tcs/2. Any I/O completion or arrival
    // that happens at time t' where last_preempt_t <= t' < last_preempt_t + tcs/2
    // should be inserted BEFORE the preempted process in the queue.
    int last_preempt_t = -1;
    bool in_preempt_cs = false;

    printf("time 0ms: Simulator started for RR [Q: -]\n");

    while (terminated < n)
    {
        Event e = dequeue_event(events, &evt_size);
        t = e.time;
        int pi = e.proc_idx;

        // close the preempt cs window once we move past it
        if (in_preempt_cs && t >= last_preempt_t + tcs / 2)
            in_preempt_cs = false;

        if (e.type == EVT_ARRIVAL)
        {
            // RR: no preemption on arrival — push to FIFO queue
            // but if we're in the cs-out window of a preemption, the preempted
            // process at queue tail hasn't actually entered yet, so this arrival
            // goes BEFORE it (i.e. position size-1 before final push)
            if (in_preempt_cs && rq_size > 0 && t < last_preempt_t + tcs / 2)
            {
                // insert at position rq_size-1 (before the preempted process at tail)
                rq[rq_size] = rq[rq_size - 1];
                rq[rq_size - 1] = pi;
                rq_size++;
            }
            else
            {
                rq_push(rq, &rq_size, pi);
            }
            wait_start[pi] = t;
            if (t <= 9999)
            {
                printf("time %dms: Process %s arrived; added to ready queue ", t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            if (cpu_proc == -1 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int cs_start = t;
                int start_time = cs_start + tcs / 2;
                int waited = cs_start - wait_start[next];
                bool is_fresh_rr = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    if (is_fresh_rr) bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    if (is_fresh_rr) bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
            }
        }
        else if (e.type == EVT_CPU_START)
        {
            cpu_proc = pi;
            cpu_next = -1;
            cpu_run_start = t;
            in_preempt_cs = false;
            int full_burst = procs[pi].cpu_bursts[burst_idx[pi]];
            if (t <= 9999)
            {
                if (remaining[pi] == full_burst)
                    printf("time %dms: Process %s started using the CPU for %dms burst ",
                           t, procs[pi].id, remaining[pi]);
                else
                    printf("time %dms: Process %s started using the CPU for remaining %dms of %dms burst ",
                           t, procs[pi].id, remaining[pi], full_burst);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            // schedule both CPU completion (at remaining time) and time-slice expiry
            int run_time = (remaining[pi] < slice) ? remaining[pi] : slice;
            cpu_busy_time += run_time;
            if (remaining[pi] <= slice)
            {
                // burst completes within (or exactly at) one slice — only schedule CPU_DONE
                Event cd = {t + remaining[pi], EVT_CPU_DONE, pi};
                enqueue_event(events, &evt_size, cd);
            }
            else
            {
                // burst will be preempted by time slice
                Event ts = {t + slice, EVT_TIMESLICE, pi};
                enqueue_event(events, &evt_size, ts);
            }
        }
        else if (e.type == EVT_TIMESLICE)
        {
            // time slice expired while pi is running
            int time_used = t - cpu_run_start;
            remaining[pi] -= time_used;

            if (rq_size == 0)
            {
                // no other processes ready; keep running, don't context switch
                if (t <= 9999)
                {
                    printf("time %dms: Time slice expired; no preemption because ready queue is empty ", t);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                cpu_run_start = t;
                int run_time = (remaining[pi] < slice) ? remaining[pi] : slice;
                cpu_busy_time += run_time;
                if (remaining[pi] <= slice)
                {
                    Event cd = {t + remaining[pi], EVT_CPU_DONE, pi};
                    enqueue_event(events, &evt_size, cd);
                }
                else
                {
                    Event ts = {t + slice, EVT_TIMESLICE, pi};
                    enqueue_event(events, &evt_size, ts);
                }
            }
            else
            {
                // preempt: print message before pushing to queue
                cpu_proc = -1;
                cpu_free_at = t + tcs / 2;
                printf("time %dms: Time slice expired; preempting process %s with %dms remaining ",
                       t, procs[pi].id, remaining[pi]);
                print_queue(rq, rq_size, procs);
                printf("\n");
                rq_push(rq, &rq_size, pi);
                wait_start[pi] = cpu_free_at;
                preemptions++;
                if (procs[pi].is_cpu_bound)
                    preempt_cpu++;
                else
                    preempt_io++;
                in_preempt_cs = true;
                last_preempt_t = t;

                // schedule next from queue
                if (cpu_next == -1 && cpu_proc == -1)
                {
                    int next = rq_pop(rq, &rq_size);
                    int start_time = cpu_free_at + tcs / 2;
                    int waited = cpu_free_at - wait_start[next];
                    bool is_fresh = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                    if (procs[next].is_cpu_bound)
                    {
                        total_wait_cpu += waited;
                        if (is_fresh) bursts_cpu++;
                        ctx_cpu++;
                    }
                    else
                    {
                        total_wait_io += waited;
                        if (is_fresh) bursts_io++;
                        ctx_io++;
                    }
                    ctx_switches++;
                    cpu_next = next;
                    Event cs = {start_time, EVT_CPU_START, next};
                    enqueue_event(events, &evt_size, cs);
                }
            }
        }
        else if (e.type == EVT_CPU_DONE)
        {
            // burst fully complete (CPU_DONE only fires for actual completion now)
            cpu_proc = -1;
            // cpu_free_at only set when going to I/O (not on termination)
            int bi = burst_idx[pi];
            int bursts_left = procs[pi].num_bursts - bi - 1;

            // count "completed within one time slice" if burst time <= slice
            if (procs[pi].cpu_bursts[bi] <= slice)
            {
                if (procs[pi].is_cpu_bound) slice_complete_cpu++;
                else slice_complete_io++;
            }

            double turn = (double)(t + tcs / 2) - arrival_for_burst[pi];
            if (procs[pi].is_cpu_bound)
                total_turn_cpu += turn;
            else
                total_turn_io += turn;

            if (bursts_left > 0 && t <= 9999)
            {
                printf("time %dms: Process %s completed a CPU burst; %d burst%s to go ",
                       t, procs[pi].id, bursts_left, bursts_left == 1 ? "" : "s");
                print_queue(rq, rq_size, procs);
                printf("\n");
            }

            if (bursts_left == 0)
            {
                terminated++;
                sim_end = t + tcs / 2;
                printf("time %dms: Process %s terminated ", t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            else
            {
                // go to I/O — first half of cs-out blocks future I/O completions
                cpu_free_at = t + tcs / 2;
                int io_done = cpu_free_at + procs[pi].io_bursts[bi];
                if (t <= 9999)
                {
                    printf("time %dms: Process %s switching out of CPU; blocking on I/O until time %dms ",
                           t, procs[pi].id, io_done);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                burst_idx[pi]++;
                remaining[pi] = procs[pi].cpu_bursts[burst_idx[pi]];
                Event io_done_evt = {io_done, EVT_IO_DONE, pi};
                enqueue_event(events, &evt_size, io_done_evt);
            }

            // schedule next from queue only when CPU is truly idle
            // (use t+tcs/2 — works for both terminate and I/O)
            if (rq_size > 0 && cpu_next == -1 && cpu_proc == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int next_cs_can_start = t + tcs / 2;
                int start_time = next_cs_can_start + tcs / 2;
                int waited = next_cs_can_start - wait_start[next];
                bool is_fresh = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    if (is_fresh) bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    if (is_fresh) bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
            }
        }
        else if (e.type == EVT_IO_DONE)
        {
            // RR: no preemption on I/O return — push to FIFO queue
            // but if we're in the cs-out window of a preemption, the preempted
            // process at the queue tail hasn't actually entered yet, so this
            // I/O completion goes BEFORE it
            remaining[pi] = procs[pi].cpu_bursts[burst_idx[pi]];
            if (in_preempt_cs && rq_size > 0 && t < last_preempt_t + tcs / 2)
            {
                rq[rq_size] = rq[rq_size - 1];
                rq[rq_size - 1] = pi;
                rq_size++;
            }
            else
            {
                rq_push(rq, &rq_size, pi);
            }
            wait_start[pi] = t;
            arrival_for_burst[pi] = t;
            if (t <= 9999)
            {
                printf("time %dms: Process %s completed I/O; added to ready queue ", t, procs[pi].id);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
            if (cpu_proc == -1 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int cs_start = t;
                int start_time = cs_start + tcs / 2;
                int waited = cs_start - wait_start[next];
                bool is_fresh = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    if (is_fresh) bursts_cpu++;
                    ctx_cpu++;
                }
                else
                {
                    total_wait_io += waited;
                    if (is_fresh) bursts_io++;
                    ctx_io++;
                }
                ctx_switches++;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
            }
        }
    }

    printf("time %dms: Simulator ended for RR ", sim_end);
    print_queue(rq, rq_size, procs);
    printf("\n");

    out->total_wait_cpu = total_wait_cpu;
    out->total_wait_io = total_wait_io;
    out->total_turn_cpu = total_turn_cpu;
    out->total_turn_io = total_turn_io;
    out->bursts_cpu = bursts_cpu;
    out->bursts_io = bursts_io;
    out->ctx_switches = ctx_switches;
    out->ctx_cpu = ctx_cpu;
    out->ctx_io = ctx_io;
    out->preemptions = preemptions;
    out->preempt_cpu = preempt_cpu;
    out->preempt_io = preempt_io;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
    out->slice_complete_cpu = slice_complete_cpu;
    out->slice_complete_io = slice_complete_io;
}

int main(int argc, char **argv)
{

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 9)
    {
        fprintf(stderr, "ERROR: Invalid arguments\n");
        fprintf(stderr, "USAGE: proj1.out <n> <ncpu> <seed> <lambda>  <threshold> <tcs> <alpha>\n");
        abort();
    }

    // initilize arg variables
    int n = atoi(argv[1]);
    if (n < 1 || n > 260)
    {
        fprintf(stderr, "ERROR: out of bounds for arg n\n");
        abort();
    }
    int ncpu = atoi(argv[2]);
    if (ncpu < 0 || ncpu > n)
    {
        fprintf(stderr, "ERROR: out of bounds for arg ncpu\n");
        abort();
    }
    int tcs = atoi(argv[6]);
    if (tcs <= 0 || tcs % 2 != 0)
    {
        fprintf(stderr, "ERROR: tcs not a valid value, must be a positive even integer\n");
        abort();
    }
    double alpha = atof(argv[7]);
    if ((alpha < 0 || alpha > 1) && alpha != -1)
    {
        fprintf(stderr, "ERROR: out of bounds for arg alpha\n");
        abort();
    }
    int slice = atoi(argv[8]);
    if (slice <= 0)
    {
        fprintf(stderr, "ERROR: out of bounds for arg slice\n");
        abort();
    }

    // rest of variables can be any value
    int seed = atoi(argv[3]);
    double lamda = atof(argv[4]);
    int upperBound = atoi(argv[5]);
    srand48(seed);

    char **processIDs = calloc(n, sizeof(char *));
    createPIDs(n, processIDs);

    if (ncpu == 1)
    {
        printf("<<< -- process set (n=%d) with %d CPU-bound process\n", n, ncpu);
    }
    else
    {
        printf("<<< -- process set (n=%d) with %d CPU-bound processes\n", n, ncpu);
    }
    printf("<<< -- seed=%d; lambda=%.6f; upper bound=%d\n", seed, lamda, upperBound);
    if (alpha == -1.0)
    {
        printf("<<< -- t_cs=%dms; alpha=<n/a>; t_slice=%dms\n", tcs, slice);
    }
    else
    {
        printf("<<< -- t_cs=%dms; alpha=%.2f; t_slice=%dms\n", tcs, alpha, slice);
    }

    // allocate array to hold all process data for simulation
    Process *procs = calloc(n, sizeof(Process));

    // go through each process and calculate cpu and i/o burst times
    printf("\n");
    for (int i = 0; i < n; i++)
    {
        int arrival, num_bursts;
        getNumBurstsarrivals(lamda, upperBound, &arrival, &num_bursts);

        int *CPU_burst_times = malloc(num_bursts * sizeof(int));
        int *IO_burst_times = malloc((num_bursts > 1 ? num_bursts - 1 : 1) * sizeof(int));
        bool is_cpu_bound = (i >= n - ncpu);

        getBursts(CPU_burst_times, IO_burst_times, lamda, upperBound, num_bursts, is_cpu_bound);

        // save all data into the Process struct for use in simulation
        strncpy(procs[i].id, processIDs[i], 3);
        procs[i].arrival_time = arrival;
        procs[i].num_bursts = num_bursts;
        procs[i].cpu_bursts = CPU_burst_times; // owned by procs[i] now
        procs[i].io_bursts = IO_burst_times;   // owned by procs[i] now
        procs[i].is_cpu_bound = is_cpu_bound;

        // print process summary line
        if (is_cpu_bound)
        {
            printf("CPU-bound process %s:", processIDs[i]);
        }
        else
        {
            printf("I/O-bound process %s:", processIDs[i]);
        }

        if (num_bursts == 1)
        {
            printf(" arrival time %dms; %d CPU burst\n", arrival, num_bursts);
        }
        else
        {
            printf(" arrival time %dms; %d CPU bursts\n", arrival, num_bursts);
        }

        free(processIDs[i]);
    }

    // Compute per-bound process stats for simout.txt header
    double total_cpu_burst_cpu = 0, total_cpu_burst_io = 0;
    double total_io_burst_cpu = 0, total_io_burst_io = 0;
    int num_cpu_bursts_cpu = 0, num_cpu_bursts_io = 0;
    int num_io_bursts_cpu = 0, num_io_bursts_io = 0;
    for (int i = 0; i < n; i++)
    {
        if (procs[i].is_cpu_bound)
        {
            for (int j = 0; j < procs[i].num_bursts; j++)
                total_cpu_burst_cpu += procs[i].cpu_bursts[j];
            num_cpu_bursts_cpu += procs[i].num_bursts;
            for (int j = 0; j < procs[i].num_bursts - 1; j++)
                total_io_burst_cpu += procs[i].io_bursts[j];
            num_io_bursts_cpu += procs[i].num_bursts - 1;
        }
        else
        {
            for (int j = 0; j < procs[i].num_bursts; j++)
                total_cpu_burst_io += procs[i].cpu_bursts[j];
            num_cpu_bursts_io += procs[i].num_bursts;
            for (int j = 0; j < procs[i].num_bursts - 1; j++)
                total_io_burst_io += procs[i].io_bursts[j];
            num_io_bursts_io += procs[i].num_bursts - 1;
        }
    }

    free(processIDs);

    printf("\n<<< PROJECT SIMULATIONS\n\n");

    Stats fcfs_s, sjf_s, srt_s, rr_s;

    fcfs(procs, n, tcs, &fcfs_s);
    printf("\n");
    sjf(procs, n, tcs, lamda, alpha, &sjf_s);
    printf("\n");
    srt(procs, n, tcs, lamda, alpha, &srt_s);
    printf("\n");
    rr(procs, n, tcs, slice, &rr_s);

    FILE *f = fopen("simout.txt", "w");
    Stats *s[4] = {&fcfs_s, &sjf_s, &srt_s, &rr_s};
    const char *names[4] = {"FCFS", (alpha == -1.0 ? "SJF-OPT" : "SJF"),
                             (alpha == -1.0 ? "SRT-OPT" : "SRT"), "RR"};

    int total_cpu_bursts = num_cpu_bursts_cpu + num_cpu_bursts_io;
    int total_io_bursts  = num_io_bursts_cpu  + num_io_bursts_io;
    double avg_cpu_b_cpu = num_cpu_bursts_cpu > 0 ? total_cpu_burst_cpu / num_cpu_bursts_cpu : 0;
    double avg_cpu_b_io  = num_cpu_bursts_io  > 0 ? total_cpu_burst_io  / num_cpu_bursts_io  : 0;
    double avg_cpu_b_all = total_cpu_bursts   > 0 ? (total_cpu_burst_cpu + total_cpu_burst_io) / total_cpu_bursts : 0;
    double avg_io_b_cpu  = num_io_bursts_cpu  > 0 ? total_io_burst_cpu  / num_io_bursts_cpu  : 0;
    double avg_io_b_io   = num_io_bursts_io   > 0 ? total_io_burst_io   / num_io_bursts_io   : 0;
    double avg_io_b_all  = total_io_bursts    > 0 ? (total_io_burst_cpu  + total_io_burst_io)  / total_io_bursts  : 0;

    fprintf(f, "-- number of processes: %d\n", n);
    fprintf(f, "-- number of CPU-bound processes: %d\n", ncpu);
    fprintf(f, "-- number of I/O-bound processes: %d\n", n - ncpu);
    fprintf(f, "-- CPU-bound average CPU burst time: %.2f ms\n", avg_cpu_b_cpu);
    fprintf(f, "-- I/O-bound average CPU burst time: %.2f ms\n", avg_cpu_b_io);
    fprintf(f, "-- overall average CPU burst time: %.2f ms\n",   avg_cpu_b_all);
    fprintf(f, "-- CPU-bound average I/O burst time: %.2f ms\n", avg_io_b_cpu);
    fprintf(f, "-- I/O-bound average I/O burst time: %.2f ms\n", avg_io_b_io);
    fprintf(f, "-- overall average I/O burst time: %.2f ms\n",   avg_io_b_all);

    for (int i = 0; i < 4; i++)
    {
        double util = s[i]->sim_end == 0 ? 0.0 : 100.0 * s[i]->cpu_busy_time / s[i]->sim_end;
        double avg_wait_cpu = s[i]->bursts_cpu > 0 ? s[i]->total_wait_cpu / s[i]->bursts_cpu : 0.0;
        double avg_wait_io  = s[i]->bursts_io  > 0 ? s[i]->total_wait_io  / s[i]->bursts_io  : 0.0;
        int    tot_b        = s[i]->bursts_cpu + s[i]->bursts_io;
        double avg_wait_all = tot_b > 0 ? (s[i]->total_wait_cpu + s[i]->total_wait_io) / tot_b : 0.0;
        double avg_turn_cpu = s[i]->bursts_cpu > 0 ? s[i]->total_turn_cpu / s[i]->bursts_cpu : 0.0;
        double avg_turn_io  = s[i]->bursts_io  > 0 ? s[i]->total_turn_io  / s[i]->bursts_io  : 0.0;
        double avg_turn_all = tot_b > 0 ? (s[i]->total_turn_cpu + s[i]->total_turn_io) / tot_b : 0.0;

        fprintf(f, "\nAlgorithm %s\n", names[i]);
        fprintf(f, "-- CPU utilization: %.2f%%\n", util);
        fprintf(f, "-- CPU-bound average wait time: %.2f ms\n", avg_wait_cpu);
        fprintf(f, "-- I/O-bound average wait time: %.2f ms\n", avg_wait_io);
        fprintf(f, "-- overall average wait time: %.2f ms\n",   avg_wait_all);
        fprintf(f, "-- CPU-bound average turnaround time: %.2f ms\n", avg_turn_cpu);
        fprintf(f, "-- I/O-bound average turnaround time: %.2f ms\n", avg_turn_io);
        fprintf(f, "-- overall average turnaround time: %.2f ms\n",   avg_turn_all);
        fprintf(f, "-- CPU-bound number of context switches: %d\n", s[i]->ctx_cpu);
        fprintf(f, "-- I/O-bound number of context switches: %d\n", s[i]->ctx_io);
        fprintf(f, "-- overall number of context switches: %d\n",   s[i]->ctx_switches);
        fprintf(f, "-- CPU-bound number of preemptions: %d\n", s[i]->preempt_cpu);
        fprintf(f, "-- I/O-bound number of preemptions: %d\n", s[i]->preempt_io);
        fprintf(f, "-- overall number of preemptions: %d\n",   s[i]->preemptions);

        if (i == 3)
        {
            double pct_cpu = s[i]->bursts_cpu > 0 ? 100.0 * s[i]->slice_complete_cpu / s[i]->bursts_cpu : 0.0;
            double pct_io  = s[i]->bursts_io  > 0 ? 100.0 * s[i]->slice_complete_io  / s[i]->bursts_io  : 0.0;
            int    sc_tot  = s[i]->slice_complete_cpu + s[i]->slice_complete_io;
            double pct_all = tot_b > 0 ? 100.0 * sc_tot / tot_b : 0.0;
            fprintf(f, "-- CPU-bound percentage of CPU bursts completed within one time slice: %.2f%%\n", pct_cpu);
            fprintf(f, "-- I/O-bound percentage of CPU bursts completed within one time slice: %.2f%%\n", pct_io);
            fprintf(f, "-- overall percentage of CPU bursts completed within one time slice: %.2f%%\n",   pct_all);
        }
    }
    fclose(f);

    // free all process data
    for (int i = 0; i < n; i++)
    {
        free(procs[i].cpu_bursts);
        free(procs[i].io_bursts);
    }
    free(procs);
    return EXIT_SUCCESS;
}