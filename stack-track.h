
#ifndef STACK_TRACK_H
#define STACK_TRACK_H 1

///////////////////////////////////////////////////////////////////////////////
// INCLUDES
///////////////////////////////////////////////////////////////////////////////
#include "htm.h"

///////////////////////////////////////////////////////////////////////////////
// DEFINES
///////////////////////////////////////////////////////////////////////////////
#define ST_MAX_THREADS (100)

#define ST_MAX_FREE_LIST (1000)

#define ST_MAX_STACKS (20)
#define ST_MAX_HP_RECORDS (100)

#define ST_MAX_OPS (20)
#define ST_MAX_SEGMENTS (1000)

// Segment adjustment parameters
#define ST_SEGMENT_MAX_HTM_ABORTS (50)
#define ST_SEGMENT_MIN_LENGTH (5)
#define ST_SEGMENT_LEN_DELTA (5)
#define ST_SEGMENT_MAX_CAPACITY_ABORTS_FOR_DEC (4)
#define ST_SEGMENT_MIN_SUCCESS_FOR_INC (4)

///////////////////////////////////////////////////////////////////////////////
// TYPES
///////////////////////////////////////////////////////////////////////////////
typedef struct _free_entry_t {
	char is_found;
	int64_t *ptr_to_free;
} free_entry_t;

typedef struct _stack_entry_t {
	long flag;
	volatile int64_t *p_start;	
	volatile int64_t *p_end;	
} stack_entry_t;

typedef struct _st_hp_record_t {
	volatile int64_t *ptr;
} st_hp_record_t;

typedef struct _st_segment_t {
	int n_limit;

	long saved_n_htm_success;
	long n_htm_success;
	
} st_segment_t;

typedef struct _st_thread_stats_t {
	long n_ops;
	long n_splits;
	long n_split_length;
	long n_stack_scans;
	long n_slow_path_segments;
	
} st_thread_stats_t;
		
typedef struct _st_thread_t { 
	int64_t uniq_id;
	int *p_seed;
		
	int op_index;
	int split_index;

	char is_htm_active;
	volatile long is_slow_path;
	volatile long split_counter;
	
	int cur_segment_len;
	int cur_segment_limit;
	int max_segment_len;	

	htm_thread_data_t *p_htm_data;	
	htm_thread_data_t htm_data;
	
	volatile long stack_counter;
	volatile long n_next_stack;
	volatile long n_stacks;
	volatile stack_entry_t stacks[ST_MAX_STACKS];

	volatile long n_hp_records;
	volatile st_hp_record_t hp_records[ST_MAX_HP_RECORDS];

	st_segment_t segments[ST_MAX_OPS][ST_MAX_SEGMENTS];

	int free_list_max_size;	
	int free_list_size;
	free_entry_t free_list[ST_MAX_FREE_LIST];

	st_thread_stats_t stats;
	
} st_thread_t ; 

///////////////////////////////////////////////////////////////////////////////
// EXTERNAL FUNCTIONS
///////////////////////////////////////////////////////////////////////////////

void ST_thread_init(st_thread_t *self, int *p_seed, int max_segment_len, int free_list_max_size);
void ST_thread_finish(st_thread_t *self);

void ST_init(st_thread_t *self);
void ST_finish(st_thread_t *self);

void ST_stack_init(st_thread_t *self);
void ST_stack_add_range(st_thread_t *self, char *p_stack, int n_bytes);
void ST_stack_publish(st_thread_t *self);

void ST_stack_del(st_thread_t *self);

void ST_split_start(st_thread_t *self, int op_index);
void ST_split_finish(st_thread_t *self);
void ST_split_segment_start(st_thread_t *self);
void ST_split_segment_finish(st_thread_t *self);

#define ST_SPLIT(self) \
	self->cur_segment_len++; \
	if (self->cur_segment_len > self->cur_segment_limit) { \
		ST_split_segment_finish(self); \
		ST_split_segment_start(self); \
	} \

void ST_HP_reset(st_thread_t *self);
volatile st_hp_record_t *ST_HP_alloc(st_thread_t *self);
void ST_HP_init(volatile st_hp_record_t *p_hp, volatile int64_t **ptr_ptr);

#define ST_HP_INIT(self, p_hp, ptr_ptr) if (unlikely(self->is_slow_path)) { ST_HP_init(p_hp, (volatile int64_t **)ptr_ptr); }

void ST_free(st_thread_t *self, int64_t *ptr);

void ST_print_stats();

///////////////////////////////////////////////////////////////////////////////
// HELPERS
///////////////////////////////////////////////////////////////////////////////

#endif // STACK_TRACK_H
