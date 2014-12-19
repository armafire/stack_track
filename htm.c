
#include <stdio.h>
#include <string.h>
#include "atomics.h"
#include "htm.h"

#ifndef __x86_64
#error currently supports only x86 64 bit
#endif

/////////////////////////////////////////////////////////
// TYPES
/////////////////////////////////////////////////////////

typedef struct htm_data {

	volatile long n_xabort_explicit;
	volatile long n_xabort_retry;
	volatile long n_xabort_conflict;
	volatile long n_xabort_capacity;
	volatile long n_xabort_debug;
	volatile long n_xabort_nested;

} htm_data_t;

/////////////////////////////////////////////////////////
// GLOBALS
/////////////////////////////////////////////////////////

htm_data_t g_htm_data = {0,};

/////////////////////////////////////////////////////////
// INTERNAL FUNCTIONS
/////////////////////////////////////////////////////////

void HTM_status_collect(htm_thread_data_t *self, unsigned int status)
{
	if (status & _XABORT_EXPLICIT)
	{
		self->n_xabort_explicit++;
	}
	if (status & _XABORT_RETRY)
	{
		self->n_xabort_retry++;
	}
	if (status & _XABORT_CONFLICT)
	{
		self->n_xabort_conflict++;
	}
	if (status & _XABORT_CAPACITY)
	{
		self->n_xabort_capacity++;
	}
	if (status & _XABORT_DEBUG)
	{
		self->n_xabort_debug++;
	}
	if (status & _XABORT_NESTED)
	{
		self->n_xabort_nested++;
	}
	
}

/////////////////////////////////////////////////////////
// EXTERNAL FUNCTIONS
/////////////////////////////////////////////////////////
void HTM_init() {}

void HTM_finish() {}

void HTM_thread_init(htm_thread_data_t *self) {
	memset(self, 0, sizeof(htm_thread_data_t));
}

void HTM_thread_finish(htm_thread_data_t *self) {
	atomic_add(&(g_htm_data.n_xabort_explicit), self->n_xabort_explicit);
	atomic_add(&(g_htm_data.n_xabort_conflict), self->n_xabort_conflict);
	atomic_add(&(g_htm_data.n_xabort_capacity), self->n_xabort_capacity);
	atomic_add(&(g_htm_data.n_xabort_debug), self->n_xabort_debug);
	atomic_add(&(g_htm_data.n_xabort_nested), self->n_xabort_nested);
}

int HTM_start(htm_thread_data_t *self) {
	unsigned int status = 0;
	status = _xbegin();
	if (status != _XBEGIN_STARTED)
	{
		self->last_htm_abort = status;
		HTM_status_collect(self, status);
		return 0;
	}
	return 1;
}

void HTM_commit() {
	_xend();
}

void HTM_print_stats() {
	printf("-------------------------------------------------\n");
	printf("  HTM aborts status:\n");
	printf("    t_htm_conflict = %lu\n", g_htm_data.n_xabort_conflict);
	printf("    t_htm_capacity = %lu\n", g_htm_data.n_xabort_capacity);
	printf("    t_htm_explicit = %lu\n", g_htm_data.n_xabort_explicit);
	printf("-------------------------------------------------\n");
}


