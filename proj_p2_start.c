/*
Operating Systems Project part 1
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
#define EVT_CPU_DONE 0
#define EVT_CPU_START 1
#define EVT_IO_DONE 2
#define EVT_ARRIVAL 3

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
    int preemptions;
    int cpu_busy_time;
    int sim_end;
} Stats;

void fcfs(Process *procs, int n, int tcs, Stats *out)
{
    // per-process simulation state
    int burst_idx[260] = {0}; // which burst each process is on

    // stats
    int wait_start[260]; // time process entered ready queue
    double total_wait_cpu = 0, total_wait_io = 0;
    double total_turn_cpu = 0, total_turn_io = 0;
    int bursts_cpu = 0, bursts_io = 0;
    int ctx_switches = 0;
    int ctx_cpu = 0, ctx_io = 0;
    int cpu_busy_time = 0;
    int arrival_for_burst[260]; // when this burst's "arrival" was
    int sim_end = 0;            // set when last process terminates

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
                int cs_start = (t >= cpu_free_at) ? t : cpu_free_at; // respect any ongoing switch-out
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

            if (t <= 9999)
            {
                printf("time %dms: Process %s completed a CPU burst; %d burst%s to go ",
                       t, procs[pi].id, bursts_left, bursts_left == 1 ? "" : "s");
                print_queue(rq, rq_size, procs);
                printf("\n");
            }

            cpu_proc = -1;
            cpu_free_at = t + tcs / 2; // first half of context switch out

            if (bursts_left == 0)
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
                // go to I/O
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
            if (rq_size > 0 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int start_time = cpu_free_at + tcs / 2; // switch-out done + switch-in
                int waited = cpu_free_at - wait_start[next];
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
                int cs_start = (t >= cpu_free_at) ? t : cpu_free_at;
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
    out->preemptions = 0;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
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
                int cs_start = (t >= cpu_free_at) ? t : cpu_free_at;
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
            cpu_free_at = t + tcs / 2;

            if (t <= 9999)
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

            // recalculate tau after burst (sjf only, not opt); print before switching out
            if (alpha != -1.0)
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
                // go to I/O
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
            if (rq_size > 0 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int start_time = cpu_free_at + tcs / 2;
                int waited = cpu_free_at - wait_start[next];
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
                int cs_start = (t >= cpu_free_at) ? t : cpu_free_at;
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
    out->preemptions = 0;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
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
void srt(Process *procs, int n, int tcs, double lambda, double alpha)
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
    for (int i = 0; i < n; i++)
    {
        tau[i] = (alpha == -1.0) ? procs[i].cpu_bursts[0] : tau0;
        arrival_for_burst[i] = procs[i].arrival_time;
        remaining[i] = procs[i].cpu_bursts[0]; // actual first burst
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
            wait_start[pi] = t;

            if (cpu_proc != -1)
            {
                // CPU busy — check preemption BEFORE inserting pi
                int time_used = t - cpu_run_start;
                int left = remaining[cpu_proc] - time_used;

                if (tau[pi] < left)
                {
                    // PREEMPT: pi goes straight to CPU, never enters queue
                    if (t <= 9999)
                    {
                        if (alpha == -1.0)
                            printf("time %dms: Process %s arrived; preempting %s (remaining time %dms) ", t, procs[pi].id, procs[cpu_proc].id, left);
                        else
                            printf("time %dms: Process %s (tau %dms) arrived; preempting %s (predicted remaining time %dms) ", t, procs[pi].id, tau[pi], procs[cpu_proc].id, left);
                        print_queue(rq, rq_size, procs);
                        printf("\n");
                    }

                    cancel_event(events, &evt_size, cpu_proc, EVT_CPU_DONE);

                    remaining[cpu_proc] = left;
                    wait_start[cpu_proc] = t + tcs / 2;
                    rq_insert_srt(rq, &rq_size, cpu_proc, remaining);

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

                    int burst_len = remaining[pi];
                    cpu_busy_time += burst_len;
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
                    rq_insert_srt(rq, &rq_size, pi, remaining);
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
                rq_insert_srt(rq, &rq_size, pi, remaining);
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
                    int cs_start = (t >= cpu_free_at) ? t : cpu_free_at;
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
                    int burst_len = remaining[next];
                    cpu_busy_time += burst_len;
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
            cpu_proc = pi;
            cpu_next = -1;
            cpu_run_start = t;
            if (t <= 9999)
            {
                if (alpha == -1.0)
                {
                    printf("time %dms: Process %s started using the CPU for %dms burst ",
                           t, procs[pi].id, remaining[pi]);
                }
                else
                {
                    printf("time %dms: Process %s (tau %dms) started using the CPU for %dms burst ",
                           t, procs[pi].id, tau[pi], remaining[pi]);
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
            cpu_free_at = t + tcs / 2;

            if (t <= 9999)
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

            // recalculate tau after burst (sjf only, not opt); print before switching out
            if (alpha != -1.0)
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
                // go to I/O
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
            if (rq_size > 0 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int start_time = cpu_free_at + tcs / 2;
                int waited = cpu_free_at - wait_start[next];
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
                int burst_len = remaining[next];
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
            remaining[pi] = procs[pi].cpu_bursts[burst_idx[pi]];
            wait_start[pi] = t;
            arrival_for_burst[pi] = t;

            if (cpu_proc != -1)
            {
                // CPU busy — check preemption BEFORE inserting pi
                int time_used = t - cpu_run_start;
                int left = remaining[cpu_proc] - time_used;

                if (tau[pi] < left)
                {
                    // PREEMPT: pi goes straight to CPU, never enters queue
                    if (t <= 9999)
                    {
                        if (alpha == -1.0)
                            printf("time %dms: Process %s completed I/O; preempting %s (remaining time %dms) ", t, procs[pi].id, procs[cpu_proc].id, left);
                        else
                            printf("time %dms: Process %s (tau %dms) completed I/O; preempting %s (predicted remaining time %dms) ", t, procs[pi].id, tau[pi], procs[cpu_proc].id, left);
                        print_queue(rq, rq_size, procs);
                        printf("\n");
                    }

                    cancel_event(events, &evt_size, cpu_proc, EVT_CPU_DONE);

                    remaining[cpu_proc] = left;
                    wait_start[cpu_proc] = t + tcs / 2;
                    rq_insert_srt(rq, &rq_size, cpu_proc, remaining);

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

                    int burst_len = remaining[pi];
                    cpu_busy_time += burst_len;
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
                    rq_insert_srt(rq, &rq_size, pi, remaining);
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
                rq_insert_srt(rq, &rq_size, pi, remaining);
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
                    int cs_start = (t >= cpu_free_at) ? t : cpu_free_at;
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
                    int burst_len = remaining[next];
                    cpu_busy_time += burst_len;
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
    out->preemptions = preemptions;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
    // TODO: store stats for simout.txt
}

void rr(Process *procs, int n, int tcs, int slice)
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

    printf("time 0ms: Simulator started for RR [Q: -]\n");

    while (terminated < n)
    {
        Event e = dequeue_event(events, &evt_size);
        t = e.time;
        int pi = e.proc_idx;

        if (e.type == EVT_ARRIVAL)
        {
            // RR: no preemption on arrival — push to back of FIFO queue
            rq_push(rq, &rq_size, pi);
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
                int cs_start = (t >= cpu_free_at) ? t : cpu_free_at;
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
                int run_time = (remaining[next] < slice) ? remaining[next] : slice;
                cpu_busy_time += run_time;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + run_time, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
        else if (e.type == EVT_CPU_START)
        {
            cpu_proc = pi;
            cpu_next = -1;
            cpu_run_start = t;
            if (t <= 9999)
            {
                printf("time %dms: Process %s started using the CPU for %dms burst ",
                       t, procs[pi].id, remaining[pi]);
                print_queue(rq, rq_size, procs);
                printf("\n");
            }
        }
        else if (e.type == EVT_CPU_DONE)
        {
            int time_used = t - cpu_run_start;
            remaining[pi] -= time_used;
            cpu_proc = -1;
            cpu_free_at = t + tcs / 2;

            if (remaining[pi] > 0)
            {
                // quantum expired — process still has burst time left
                if (t <= 9999)
                {
                    printf("time %dms: Time slice expired; process %s preempted with %dms remaining ",
                           t, procs[pi].id, remaining[pi]);
                    print_queue(rq, rq_size, procs);
                    printf("\n");
                }
                rq_push(rq, &rq_size, pi); // back to tail of FIFO queue
                wait_start[pi] = cpu_free_at;
                preemptions++;
                if (procs[pi].is_cpu_bound)
                    preempt_cpu++;
                else
                    preempt_io++;
            }
            else
            {
                // burst fully complete
                int bi = burst_idx[pi];
                int bursts_left = procs[pi].num_bursts - bi - 1;

                double turn = (double)(t + tcs / 2) - arrival_for_burst[pi];
                if (procs[pi].is_cpu_bound)
                    total_turn_cpu += turn;
                else
                    total_turn_io += turn;

                if (t <= 9999)
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
            }

            // schedule next from queue
            if (rq_size > 0 && cpu_next == -1)
            {
                int next = rq_pop(rq, &rq_size);
                int start_time = cpu_free_at + tcs / 2;
                int waited = cpu_free_at - wait_start[next];
                bool is_fresh = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    if (is_fresh)
                    {
                        bursts_cpu++;
                        ctx_cpu++;
                    }
                }
                else
                {
                    total_wait_io += waited;
                    if (is_fresh)
                    {
                        bursts_io++;
                        ctx_io++;
                    }
                }
                ctx_switches++;
                int run_time = (remaining[next] < slice) ? remaining[next] : slice;
                cpu_busy_time += run_time;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + run_time, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
        else if (e.type == EVT_IO_DONE)
        {
            // RR: no preemption on I/O return — push to back of FIFO queue
            remaining[pi] = procs[pi].cpu_bursts[burst_idx[pi]];
            rq_push(rq, &rq_size, pi);
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
                int cs_start = (t >= cpu_free_at) ? t : cpu_free_at;
                int start_time = cs_start + tcs / 2;
                int waited = cs_start - wait_start[next];
                bool is_fresh = (remaining[next] == procs[next].cpu_bursts[burst_idx[next]]);
                if (procs[next].is_cpu_bound)
                {
                    total_wait_cpu += waited;
                    if (is_fresh)
                    {
                        bursts_cpu++;
                        ctx_cpu++;
                    }
                }
                else
                {
                    total_wait_io += waited;
                    if (is_fresh)
                    {
                        bursts_io++;
                        ctx_io++;
                    }
                }
                ctx_switches++;
                int run_time = (remaining[next] < slice) ? remaining[next] : slice;
                cpu_busy_time += run_time;
                cpu_next = next;
                Event cs = {start_time, EVT_CPU_START, next};
                enqueue_event(events, &evt_size, cs);
                Event cd = {start_time + run_time, EVT_CPU_DONE, next};
                enqueue_event(events, &evt_size, cd);
            }
        }
    }

    printf("time %dms: Simulator ended for RR ", sim_end);
    print_queue(rq, rq_size, procs);
    printf("\n");

    // TODO: store stats for simout.txt
    out->total_wait_cpu = total_wait_cpu;
    out->total_wait_io = total_wait_io;
    out->total_turn_cpu = total_turn_cpu;
    out->total_turn_io = total_turn_io;
    out->bursts_cpu = bursts_cpu;
    out->bursts_io = bursts_io;
    out->ctx_switches = ctx_switches;
    out->preemptions = preemptions;
    out->cpu_busy_time = cpu_busy_time;
    out->sim_end = sim_end;
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

    /*
    In addition to the above output (which is sent to stdout), generate an output file called simout.txt
    that contains some general statistics followed by specific statistics for each simulated algorithm.
    The file format is shown below, with # as a placeholder for actual numerical data. If calculating
    an average will cause division by 0, simply set the average to 0
    */

    free(processIDs);

    fcfs(procs, n, tcs);
    sjf(procs, n, tcs, lamda, alpha);
    srt(procs, n, tcs, lamda, alpha);
    rr(procs, n, tcs, slice);

    Stats fcfs_s, sjf_s, srt_s, rr_s;

    // Reset seed before each simulation to ensure predictability
    srand48(seed);
    fcfs(procs, n, tcs, &fcfs_s);

    srand48(seed);
    sjf(procs, n, tcs, lamda, alpha, &sjf_s);

    srand48(seed);
    srt(procs, n, tcs, lamda, alpha, &srt_s);

    srand48(seed);
    rr(procs, n, tcs, slice, &rr_s);

    FILE *f = fopen("simout.txt", "w");
    Stats *s[4] = {&fcfs_s, &sjf_s, &srt_s, &rr_s};
    char *names[4] = {"FCFS", (alpha == -1.0 ? "SJF-OPT" : "SJF"),
                      (alpha == -1.0 ? "SRT-OPT" : "SRT"), "RR"};

    for (int i = 0; i < 4; i++)
    {
        double avg_cpu_burst = s[i]->bursts_cpu == 0 ? 0 : (double)s[i]->cpu_busy_time / (s[i]->bursts_cpu + s[i]->bursts_io);
        double avg_wait = (s[i]->bursts_cpu + s[i]->bursts_io) == 0 ? 0 : (s[i]->total_wait_cpu + s[i]->total_wait_io) / (s[i]->bursts_cpu + s[i]->bursts_io);
        double avg_turn = (s[i]->bursts_cpu + s[i]->bursts_io) == 0 ? 0 : (s[i]->total_turn_cpu + s[i]->total_turn_io) / (s[i]->bursts_cpu + s[i]->bursts_io);
        double utilization = s[i]->sim_end == 0 ? 0 : 100.0 * s[i]->cpu_busy_time / s[i]->sim_end;

        fprintf(f, "Algorithm %s\n", names[i]);
        fprintf(f, "-- average CPU burst time: %.3f ms\n", avg_cpu_burst);
        fprintf(f, "-- average wait time: %.3f ms\n", avg_wait);
        fprintf(f, "-- average turnaround time: %.3f ms\n", avg_turn);
        fprintf(f, "-- total number of context switches: %d\n", s[i]->ctx_switches);
        fprintf(f, "-- total number of preemptions: %d\n", s[i]->preemptions);
        fprintf(f, "-- CPU utilization: %.3f%%\n", utilization);
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
