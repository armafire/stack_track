
#include "common.h"

/////////////////////////////////////////////////////////
// TYPES
/////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////
// GLOBALS
/////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////
// INTERNAL FUNCTIONS
/////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////
// EXTERNAL FUNCTIONS
/////////////////////////////////////////////////////////
int MarsagliaXOR(int *p_seed) {
	int seed = *p_seed;
	
	if (seed == 0) {
		seed = 1; 
	}
	
	seed ^= seed << 6;
	seed ^= ((unsigned)seed) >> 21;
	seed ^= seed << 7; 
	
	*p_seed = seed;
	
	return seed & 0x7FFFFFFF;
}

