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

/*For each of the n processes, in order A0 through Z9, perform the steps below, with I/O-bound
processes generated first. Note that all generated values are integers, even though we use a floatingpoint pseudo-random number generator.
Define your exponential distribution pseudo-random number generation function as next_exp()
(or another similar name) and have it return the next pseudo-random number in the sequence,
capped by *(argv+5).
*/
void createPIDs(int n, char **processIDs) {
    char letter = 'A';
    for (int i = 0; i < n; i++) {
        processIDs[i] = malloc(4); // Space for 'A0\0'
        sprintf(processIDs[i], "%c%d", letter + (i / 10), i % 10);
    }
}

/*2. Identify the number of CPU bursts for the given process as the “ceiling” of the next random
number generated from the uniform distribution obtained via drand48(), then multiplied
by 16; this should obtain a random integer in the inclusive range [1, 16]*/        
double next_exp(double lamda, int upperBound) {
    double val = upperBound;
    while (val >= upperBound) {
        double r = drand48();
        val = -log(r) / lamda;
    }
    return val;
}

void getNumBurstsarrivals(double lamda, int upperBound, int* arrival, int* num_bursts) {
    *arrival = (int)floor(next_exp(lamda, upperBound));
    *num_bursts = (int)ceil(drand48() * 16); // Use drand48() for uniform dist
}

/*3. For each of CPU burst in turn, identify the CPU burst time and the I/O burst time as
the “ceiling” of the next two pseudo-random numbers given by next_exp(); for I/O-bound
processes, multiply the I/O burst time by 8 such that I/O burst time is close to an order of
magnitude longer than CPU burst time; as noted above, for CPU-bound processes, multiply
the CPU burst time by 6; and for the last CPU burst, do not generate a corresponding I/O
burst time, since each process ends with one final CPU burst*/
void getBursts(int *CPU_burst_times, int *IO_burst_times, double lamda, int upperBound, int num_bursts, bool is_cpu_bound) {
    for (int i = 0; i < num_bursts - 1; i++) {
        CPU_burst_times[i] = (int)ceil(next_exp(lamda, upperBound)) * (is_cpu_bound ? 6 : 1);
        IO_burst_times[i]  = (int)ceil(next_exp(lamda, upperBound)) * (is_cpu_bound ? 1 : 8);
    }
    CPU_burst_times[num_bursts - 1] = (int)ceil(next_exp(lamda, upperBound)) * (is_cpu_bound ? 6 : 1);
}

int main( int argc, char ** argv )
{
  
    setvbuf(stdout, NULL, _IONBF, 0);

    if ( argc != 6 ) {
        fprintf( stderr, "ERROR: Invalid arguments\n" );
        fprintf( stderr, "USAGE: proj1.out <n> <ncpu> <seed> <lambda>  <threshold> <tcs> <alpha>\n" );
        abort();
    }
    
    // initilize arg variables
    int n = atoi(argv[1]);
    if ( n < 1 || n > 260) {
        fprintf( stderr, "ERROR: out of bounds for arg n\n" );
        abort();
    }
    int ncpu = atoi(argv[2]);
    if (ncpu < 0 || ncpu > n) {
        fprintf( stderr, "ERROR: out of bounds for arg ncpu\n" );
        abort();
    }
    int tcs = atoi(argv[6]);
    if (tcs <= 0 || tcs % 2 != 0) {
        fprintf( stderr, "ERROR: tcs not a valid value, must be a positive even integer\n" );
    }
    double alpha = atof(argv[7]);
    if ((alpha < 0 || alpha > 1) && alpha != -1) {
        fprintf( stderr, "ERROR: out of bounds for arg alpha\n" );
    }
    int slice = atoi(argv[8]);
    if (slice <= 0) {
        fprintf(stderr, "ERROR: out of bounds for arg slice\n", );
    }

    // rest of variables can be any value
    int seed = atoi(argv[3]);   
    double lamda = atof(argv[4]);
    int upperBound = atoi(argv[5]);
    srand48(seed);

    char ** processIDs = calloc( n, sizeof(char*));
    createPIDs( n, processIDs);

    if (ncpu == 1) { printf("<<< -- process set (n=%d) with %d CPU-bound process\n", n, ncpu); }
    else { printf("<<< -- process set (n=%d) with %d CPU-bound processes\n", n, ncpu); }
    printf("<<< -- seed=%d; lambda=%.6f; upper bound=%d\n", seed, lamda, upperBound);

    // go through each process and calculate cpu and i/o burst times
    for( int i = 0; i < n; i++){
        printf("\n");
        
        int arrival, num_bursts;
        getNumBurstsarrivals(lamda, upperBound, &arrival, &num_bursts);
        

        int *CPU_burst_times = malloc(num_bursts * sizeof(int));
        int *IO_burst_times = malloc((num_bursts - 1) * sizeof(int));
        bool is_cpu_bound = (i >= n-ncpu);

        if (is_cpu_bound) { printf( "CPU-bound process %s:", processIDs[i] ); }
        else { printf( "I/O-bound process %s:", processIDs[i] ); } 

        if (num_bursts == 1) { printf(" arrival time %dms; %d CPU burst:\n", arrival, num_bursts); }
        else { printf(" arrival time %dms; %d CPU bursts:\n", arrival, num_bursts); }

        // Pass the pointers directly
        getBursts(CPU_burst_times, IO_burst_times, lamda, upperBound, num_bursts, is_cpu_bound);
        
        // print burst results 
        // more cpu bursts than i/o bursts so add final cpu birst outside of print loop        
        for (int i = 0; i < num_bursts-1; i++) {
            printf("==> CPU burst %dms ==> I/O burst %dms\n", CPU_burst_times[i], IO_burst_times[i]);
        }
        printf("==> CPU burst %dms\n", CPU_burst_times[num_bursts-1]);

        free(CPU_burst_times);
        free(IO_burst_times);
        free(processIDs[i]);
    }

    /*
    In addition to the above output (which is sent to stdout), generate an output file called simout.txt
    that contains some general statistics followed by specific statistics for each simulated algorithm.
    The file format is shown below, with # as a placeholder for actual numerical data. If calculating
    an average will cause division by 0, simply set the average to 0
    */

    free(processIDs);
    return EXIT_SUCCESS;
}
