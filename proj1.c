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




