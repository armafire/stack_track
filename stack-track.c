

///////////////////////////////////////////////////////////////////////////////
// INCLUDES
///////////////////////////////////////////////////////////////////////////////
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>

#include "common.h"
#include "atomics.h"
#include "stack-track.h"

///////////////////////////////////////////////////////////////////////////////
// DEFINES
///////////////////////////////////////////////////////////////////////////////
#define ST_TRACE(format, ...) //printf(format, __VA_ARGS__)

///////////////////////////////////////////////////////////////////////////////
// TYPES
///////////////////////////////////////////////////////////////////////////////
typedef struct _st_stats_t {
	long n_ops;
	long n_splits;
	long n_split_length;
	long n_stack_scans;
	long n_slow_path_segments;
	
} st_stats_t;

///////////////////////////////////////////////////////////////////////////////
// GLOBALS
///////////////////////////////////////////////////////////////////////////////
static volatile long g_uniq_id = 0;
static volatile long g_n_threads = 0;
static volatile st_thread_t *g_st_threads[ST_MAX_THREADS];

static volatile st_stats_t g_st_stats;

///////////////////////////////////////////////////////////////////////////////
// Stack Track - Thread Management
///////////////////////////////////////////////////////////////////////////////
void ST_thread_init(st_thread_t *self, int *p_seed, int max_segment_len, int free_list_max_size) {
	int i;
	int j;
	
	memset(self, 0, sizeof(st_thread_t));

	self->uniq_id = atomic_add(&g_uniq_id, 1);
	
	self->p_seed = p_seed;
		
	self->max_segment_len = max_segment_len;	
	self->free_list_max_size = free_list_max_size;
	 
	self->segments[ST_MAX_OPS][ST_MAX_SEGMENTS];
	
	for (i = 0; i < ST_MAX_OPS; i++) {
		for (j = 0; j < ST_MAX_SEGMENTS; j++) {
			self->segments[i][j].n_limit = max_segment_len;
		}
	}
	
	self->p_htm_data = &(self->htm_data);
	HTM_thread_init(self->p_htm_data);
		
	g_st_threads[self->uniq_id] = self;
	atomic_add(&g_n_threads, 1);
	
}

void ST_thread_finish(st_thread_t *self) {
	HTM_thread_finish(self->p_htm_data);
	
	atomic_add(&(g_st_stats.n_ops), self->stats.n_ops);
	atomic_add(&(g_st_stats.n_splits), self->stats.n_splits);
	atomic_add(&(g_st_stats.n_split_length), self->stats.n_split_length);
	atomic_add(&(g_st_stats.n_stack_scans), self->stats.n_stack_scans);
	atomic_add(&(g_st_stats.n_slow_path_segments), self->stats.n_slow_path_segments);
}

///////////////////////////////////////////////////////////////////////////////
// Stack Track - Operation Management
///////////////////////////////////////////////////////////////////////////////
void ST_init(st_thread_t *self) {
	self->is_slow_path = 1;
	self->n_stacks = 0;
	ST_HP_reset(self);
	MEMBARSTLD();
}

void ST_finish(st_thread_t *self) {
	self->n_stacks = 0;
	self->n_hp_records = 0;	
	
	self->stack_counter++;
	
	if (self->is_slow_path) {
		self->is_slow_path = 0;
	}
	
	MEMBARSTLD();
	
}

///////////////////////////////////////////////////////////////////////////////
// Stack Track - Stack Management
///////////////////////////////////////////////////////////////////////////////
void ST_stack_add(st_thread_t *self, int64_t *p_stack_start, int64_t *p_stack_end) {
	self->stacks[self->n_stacks].p_start = p_stack_start;
	self->stacks[self->n_stacks].p_end = p_stack_end;
	self->n_stacks++;
}
void ST_stack_del(st_thread_t *self) {
	self->n_stacks--;
}

///////////////////////////////////////////////////////////////////////////////
// Stack Track - Split Management
///////////////////////////////////////////////////////////////////////////////
void ST_split_start(st_thread_t *self, int op_index) {
	self->op_index = op_index;
	self->split_index = 0;
	
	if (self->is_slow_path) {
		self->is_slow_path = 0;
	}
	
	ST_split_segment_start(self);
}

void ST_split_finish(st_thread_t *self) {
	ST_split_segment_finish(self);
	self->stats.n_ops++;
}

void ST_split_segment_start(st_thread_t *self) {
	long saved_capacity_aborts;
	long new_capacity_aborts;
	long n_htm_aborts;
		
	saved_capacity_aborts = self->p_htm_data->n_xabort_capacity;

	self->cur_segment_limit = self->segments[self->op_index][self->split_index].n_limit;
	self->cur_segment_len = 0;
	
	n_htm_aborts = 0;
	
	self->is_htm_active = 1;
	while (0 == HTM_start(self->p_htm_data)) {
		self->is_htm_active = 0;

		n_htm_aborts++;
		
		new_capacity_aborts = self->p_htm_data->n_xabort_capacity - saved_capacity_aborts;
		
		if (new_capacity_aborts > 0) {
			self->segments[self->op_index][self->split_index].saved_n_htm_success = self->segments[self->op_index][self->split_index].n_htm_success;
		}
		
		if (new_capacity_aborts > ST_SEGMENT_MAX_CAPACITY_ABORTS_FOR_DEC) {
			
			if (self->segments[self->op_index][self->split_index].n_limit > ST_SEGMENT_MIN_LENGTH) {
				self->segments[self->op_index][self->split_index].n_limit -= ST_SEGMENT_LEN_DELTA;
			}
			
			saved_capacity_aborts = self->p_htm_data->n_xabort_capacity;
			self->cur_segment_limit = self->segments[self->op_index][self->split_index].n_limit;
		}
		
		self->cur_segment_len = 0;
		
		if (n_htm_aborts > ST_SEGMENT_MAX_HTM_ABORTS) {
			self->is_slow_path = 1;
			self->stats.n_slow_path_segments++;
			return;
		}
		
	}
	
}

void ST_split_segment_finish(st_thread_t *self) {
	long new_success;
		
	self->split_counter++;
	
	if (unlikely(self->is_slow_path)) {
		self->stats.n_splits++;
		self->stats.n_split_length += self->cur_segment_len;
		self->split_index++;
		self->is_slow_path = 0;
		MEMBARSTLD();
		return;
	}
	
	HTM_commit();
	self->is_htm_active = 0;
	
	self->segments[self->op_index][self->split_index].n_htm_success++;
	self->stats.n_splits++;
	self->stats.n_split_length += self->cur_segment_len;

	new_success = self->segments[self->op_index][self->split_index].n_htm_success - self->segments[self->op_index][self->split_index].saved_n_htm_success;
	if (new_success > ST_SEGMENT_MIN_SUCCESS_FOR_INC) {
		if (self->segments[self->op_index][self->split_index].n_limit < self->max_segment_len) {
			self->segments[self->op_index][self->split_index].n_limit += ST_SEGMENT_LEN_DELTA;
						
			self->segments[self->op_index][self->split_index].saved_n_htm_success = self->segments[self->op_index][self->split_index].n_htm_success; 
		}
	}
	
	self->split_index++;
	
}

///////////////////////////////////////////////////////////////////////////////
// StackTrack - Slow-Path - Hazard Pointers
///////////////////////////////////////////////////////////////////////////////
void ST_HP_reset(st_thread_t *self) {	
	self->n_hp_records = 0;	
}

st_hp_record_t *ST_HP_alloc(st_thread_t *self) {	
	int i;
	st_hp_record_t *p_hp;
	
	p_hp = &(self->hp_records[self->n_hp_records]);
	self->n_hp_records++;
	
	if (self->n_hp_records >= ST_MAX_HP_RECORDS) {
		abort();
	}
	
	return p_hp;
	
}

void ST_HP_init(st_hp_record_t *p_hp, volatile int64_t **ptr_ptr) {
	
	while (1) { 
		p_hp->ptr = *ptr_ptr;
		MEMBARSTLD();	
	
		if (p_hp->ptr == *ptr_ptr) {
			return;
		}
		
		CPU_RELAX;
	}
	
}

///////////////////////////////////////////////////////////////////////////////
// StackTrack - Reclamation
///////////////////////////////////////////////////////////////////////////////
int ST_scan_thread_hp_records(st_thread_t *self, int64_t *ptr_to_free) {
	int i;
		
	for (i = self->n_hp_records - 1; i >= 0; i--) {
		if (self->hp_records[i].ptr == ptr_to_free) {
			return 1;
		}
	}
	
	return 0;
}

int ST_scan_thread_stack(st_thread_t *self, int64_t *ptr_to_free) {
	int i;
	unsigned char *p;
	unsigned char *p_start;
	unsigned char *p_end;
	int64_t *ptr_val;
	
	for (i = self->n_stacks - 1; i >= 0; i--) {
		p_start = (unsigned char *)self->stacks[i].p_start;
		p_end = (unsigned char *)self->stacks[i].p_end;
		
		for (p = p_start; p < p_end; p++) {
			ptr_val = (*(int64_t **)p);
			
			if (ptr_val == ptr_to_free) {
				return 1;
			}	
		}
	}
	
	return 0;
}

int ST_scan_thread(st_thread_t *self, int64_t *ptr_to_free) {
	unsigned char *p;
	unsigned char *p_start;
	unsigned char *p_end;
	int64_t *ptr_val;
	int is_found;
	
	is_found = 0;
	
	if (unlikely(self->is_slow_path)) {
		is_found = ST_scan_thread_hp_records(self, ptr_to_free);
	}
	
	if (likely(!is_found)) {
		is_found = ST_scan_thread_stack(self, ptr_to_free);
	}
	
	return is_found;	
}

void ST_scan_and_free(st_thread_t *self) {
	int i;
	int th_id;
	volatile long local_stack_counters[ST_MAX_THREADS];
	volatile long local_split_counter;
	volatile long local_n_threads;
	int max_index;
	int cur_index;
	int n_freed;
	
	ST_TRACE("[%d] ST_scan_and_free: start\n", self->uniq_id);
	
	local_n_threads = g_n_threads;
	
	for (th_id = 0; th_id < local_n_threads; th_id++) {
		local_stack_counters[th_id] = g_st_threads[th_id]->stack_counter; 
	}
	
	for (i = 0; i < self->free_list_size; i++) {
		self->free_list[i].is_found = 0;
	}
	
	for (th_id = 0; th_id < g_n_threads; th_id++) {
		 
		for (i = 0; i < self->free_list_size; i++) {
			
			if (self->free_list[i].is_found) {
				continue;
			}
			
			if (local_stack_counters[th_id] != g_st_threads[th_id]->stack_counter) {
				break;
			}

			local_split_counter = g_st_threads[th_id]->split_counter;

			if (ST_scan_thread((st_thread_t *)g_st_threads[th_id], self->free_list[i].ptr_to_free)) {
				self->free_list[i].is_found = 1;
			}
			
			if (local_split_counter != g_st_threads[th_id]->split_counter) {
				i--; // retry the same ptr
			}
			
		}
	}
	
	max_index = self->free_list_size;
    cur_index = 0;
	n_freed = 0;
	while (cur_index < max_index) {

		if (self->free_list[cur_index].is_found) {
			cur_index++;
			continue;
		}
		
		self->free_list[cur_index] = self->free_list[max_index-1];
		n_freed++;
		max_index--;
	}
	
	ST_TRACE("[%d] ST_scan_and_free: %d nodes freed of %d\n", self->uniq_id, n_freed, self->free_list_size);

	self->free_list_size = max_index;
	
	ST_TRACE("[%d] ST_scan_and_free: finish\n", self->uniq_id);
}

void ST_free(st_thread_t *self, int64_t *ptr) {
	
	self->free_list[self->free_list_size].ptr_to_free = ptr;
	self->free_list_size++;
	
	if (self->free_list_size >= self->free_list_max_size) {
		while (self->free_list_size >= self->free_list_max_size) {
			ST_scan_and_free(self);
			self->stats.n_stack_scans++;
		}
	}
	
}

///////////////////////////////////////////////////////////////////////////////
// StackTrack - Stats
///////////////////////////////////////////////////////////////////////////////
void ST_print_stats() {
	printf("-------------------------------------------------\n");
	printf("  StackTrack status:\n");
	printf("    n_splits_per_operation = %.2f\n", (double)(g_st_stats.n_splits) / (double)(g_st_stats.n_ops));
	printf("    n_split_length = %.2f\n", (double)(g_st_stats.n_split_length) / (double)(g_st_stats.n_splits));
	printf("    n_stack_scans = %lu\n", g_st_stats.n_stack_scans);
	printf("    n_slow_path_segments = %lu\n", g_st_stats.n_slow_path_segments);
	printf("-------------------------------------------------\n");
	
}