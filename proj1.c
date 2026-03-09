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

/* 
COMMAND LINE AGRUMENTS

1.
*(argv+1): Define n as the number of processes to simulate. Process IDs are assigned a
two-character code consisting of an uppercase letter from A to Z followed by a number from
0 to 9. Processes are assigned in order A0, A1, A2, ..., A9, B0, B1, ..., Z9.

2. 
*(argv+2): Define ncpu as the number of processes that are CPU-bound. For this project,
we will classify processes as I/O-bound or CPU-bound. The ncpu CPU-bound processes,
when generated, will have CPU burst times that are longer by a factor of 6. Conversely, the
I/O-bound processes, when generated, will have I/O burst times that are longer by a factor
of 8.

3. 
*(argv+3): We will use a pseudo-random number generator to determine the interarrival
times of CPU bursts. This command-line argument, seed, serves as the initial seed value
for the pseudo-random number sequence. To ensure predictability and repeatability, use
srand48() with this given seed value before simulating each scheduling algorithm and
drand48() to obtain the next value in the range [0.0,1.0).
Since Python does not have these functions, implement an equivalent 48-bit linear congruen
tial generator, as described in the man page for these functions in C. Feel free to post your
code for this on the Discussion Forum for others to use.

4.
*(argv+4): To determine interarrival times, we will use an exponential distribution, as illus
trated in the exp-random.c example. This command-line argument is parameter λ; remember
that 1
λ will be the average random value generated, e.g., if λ = 0.01, then the average should
be appoximately 100.
In the exp-random.c example, use the formula shown in the code, i.e., −lnr / λ

5.
*(argv+5): For the exponential distribution, this command-line argument represents the
upper bound for valid pseudo-random numbers. This threshold is used to avoid values far
down the long tail of the exponential distribution. As an example, if this is set to 3000, all
generated values above 3000 should be skipped. For cases in which this value is used in the
ceiling function (see the next page), be sure the ceiling is still valid according to this upper
bound.
                                                 
*/


/*

For each of the n processes, in order A0 through Z9, perform the steps below, with I/O-bound
processes generated first. Note that all generated values are integers, even though we use a floating
point pseudo-random number generator.

Define your exponential distribution pseudo-random number generation function as next_exp()
(or another similar name) and have it return the next pseudo-random number in the sequence,
capped by *(argv+5).

1. Identify the initial process arrival time as the “floor” of the next random number in the
sequence given by next_exp(); note that you could therefore have an arrival time of 0

2. Identify the number of CPU bursts for the given process as the “ceiling” of the next random
number generated from the uniform distribution obtained via drand48(), then multiplied
by 16; this should obtain a random integer in the inclusive range [1,16]

3. For each of CPU burst in turn, identify the CPU burst time and the I/O burst time as
the “ceiling” of the next two pseudo-random numbers given by next_exp(); for I/O-bound
processes, multiply the I/O burst time by 8 such that I/O burst time is close to an order of
magnitude longer than CPU burst time; as noted above, for CPU-bound processes, multiply
the CPU burst time by 6; and for the last CPU burst, do not generate a corresponding I/O
burst time, since each process ends with one final CPU burst

*/


int main( int argc, char ** argv )
{

	setvbuf(stdout, NULL, _IONBF, 0);

  if ( argc != 6 ) {
    fprintf( stderr, "ERROR: Invalid arguments\n" );
    fprintf( stderr, "USAGE: proj1.out <n> <ncpu> <seed> <lambda>  <threshold>\n" );
    // return EXIT_FAILURE;
    abort();
  }
	
	return EXIT_SUCCESS;
}



