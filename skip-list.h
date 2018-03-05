
#ifndef SKIPLIST_H
#define SKIPLIST_H 1

///////////////////////////////////////////////////////////////////////////////
// INCLUDES
///////////////////////////////////////////////////////////////////////////////
#include "stack-track.h"

///////////////////////////////////////////////////////////////////////////////
// DEFINES
///////////////////////////////////////////////////////////////////////////////
#define SKIPLIST_MAX_LEVEL (10)

#define MIN_KEY (0)
#define MAX_KEY (1 << 28)

///////////////////////////////////////////////////////////////////////////////
// TYPES
///////////////////////////////////////////////////////////////////////////////
typedef struct _sl_node_t {
	volatile long lock;
	volatile int key;
	volatile int topLevel;
	volatile int marked;
	volatile int fullyLinked;
	volatile struct _sl_node_t *p_next[SKIPLIST_MAX_LEVEL];

} sl_node_t;

typedef struct _skiplist_t {
	volatile sl_node_t *p_head;
	volatile sl_node_t *p_tail;
	
} skiplist_t;

///////////////////////////////////////////////////////////////////////////////
// EXTERNAL FUNCTIONS
///////////////////////////////////////////////////////////////////////////////
skiplist_t *skiplist_init();

int skiplist_contains_pure(st_thread_t *self, skiplist_t *p_skiplist, int key);
int skiplist_contains_hp(st_thread_t *self, skiplist_t *p_skiplist, int key);
int skiplist_contains_stacktrack(st_thread_t *self, skiplist_t *p_skiplist, int key);
int skiplist_contains_forkscan(st_thread_t *self, skiplist_t *p_skiplist, int key);

volatile sl_node_t *skiplist_insert_pure(st_thread_t *self, skiplist_t *p_skiplist, int key);
volatile sl_node_t *skiplist_insert_hp(st_thread_t *self, skiplist_t *p_skiplist, int key);
volatile sl_node_t *skiplist_insert_stacktrack(st_thread_t *self, skiplist_t *p_skiplist, int key);
volatile sl_node_t *skiplist_insert_forkscan(st_thread_t *self, skiplist_t *p_skiplist, int key);

int skiplist_remove_pure(st_thread_t *self, skiplist_t *p_skiplist, int key);
int skiplist_remove_hp(st_thread_t *self, skiplist_t *p_skiplist, int key);
int skiplist_remove_stacktrack(st_thread_t *self, skiplist_t *p_skiplist, int key);
int skiplist_remove_forkscan(st_thread_t *self, skiplist_t *p_skiplist, int key);

int skiplist_size(skiplist_t *p_skiplist);
void skiplist_print_stats(skiplist_t *p_skiplist);

#endif // SKIPLIST_H
