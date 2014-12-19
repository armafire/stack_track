
#include "atomics.h"

/////////////////////////////////////////////////////////
// EXTERNAL FUNCTIONS
/////////////////////////////////////////////////////////
volatile int64_t atomic_add(volatile int64_t * addr, int64_t dx) { 
	volatile int64_t v;

	for (v = *addr ; CAS64 (addr, v, v+dx) != v; v = *addr) {}

	return v ; 
	
	
}

volatile int64_t CAS(volatile int64_t * addr, int64_t expected_value, int64_t new_value) {
	return CAS64(addr, expected_value, new_value);
	
}
