
/////////////////////////////////////////////////////////
// INCLUDES
/////////////////////////////////////////////////////////
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>

#include "common.h"
#include "atomics.h"
#include "skip-list.h"

/////////////////////////////////////////////////////////
// DEFINES
/////////////////////////////////////////////////////////
#define OP_ID_CONTAINS (0)
#define OP_ID_INSERT (1)
#define OP_ID_REMOVE (2)

#define SL_TRACE(format, ...) //printf(format, __VA_ARGS__)
#define SL_TRACE_IN_HTM(format, ...) //printf(format, __VA_ARGS__)

/////////////////////////////////////////////////////////
// INTERNAL FUNCTIONS
/////////////////////////////////////////////////////////
static void sl_node_lock_slow_path(st_thread_t *self, volatile sl_node_t *p_node) {
	
	while (1) {
		volatile long cur_lock = p_node->lock;
		
		if (likely(cur_lock == 0)) {
			
			if (likely(CAS(&(p_node->lock), 0, 1) == 0)) {
				return;
			}
		}
		
		CPU_RELAX;
	}
}

static void sl_node_lock(st_thread_t *self, volatile sl_node_t *p_node) {
	SL_TRACE_IN_HTM("[%d] lock: %p\n", (int)self->uniq_id, p_node);
	
	if (unlikely(!self->is_htm_active)) {
		sl_node_lock_slow_path(self, p_node);
		return;
	}
	
	if (unlikely(p_node->lock != 0)) {
		_xabort(123);
	}

	p_node->lock = 1;
	
	SL_TRACE_IN_HTM("[%d] lock: success\n", (int)self->uniq_id);
	
	return;
}



static void sl_node_unlock(st_thread_t *self, volatile sl_node_t *p_node) {
	SL_TRACE_IN_HTM("[%d] unlock: %p\n", (int)self->uniq_id, p_node);
	
	p_node->lock = 0;
	
}

static int sl_randomLevel(int *p_seed)
{
	int level = 1;
	while (MY_RAND(p_seed) % 2 == 0 && level < SKIPLIST_MAX_LEVEL) {
		level++;
	}
	return level-1;
}

static volatile sl_node_t *sl_node_alloc() {
	volatile sl_node_t *p_node;
	
	p_node = (volatile sl_node_t *)malloc(sizeof(sl_node_t));
	if (p_node == NULL) {
		abort();
	}
	
	return p_node;
}

static void sl_node_init(st_thread_t *self, volatile sl_node_t *p_node, int key, int height) {
	p_node->key = key;
	p_node->topLevel = height;
	p_node->lock = 0;
	p_node->marked = 0;
	p_node->fullyLinked = 0;
	
	if (self != NULL) {
		SL_TRACE_IN_HTM("[%d] sl_node_init: key = %d, height = %d\n", (int)self->uniq_id, key, height);
	}
	
}

static int sl_find_pure(st_thread_t *self, 
				        skiplist_t *p_skiplist, int key, 
				        volatile sl_node_t **p_preds, volatile sl_node_t **p_succs)
{
	int64_t stack_end;
	int level;
	int l_found = -1;
	volatile sl_node_t *p_pred = NULL;
	volatile sl_node_t *p_curr = NULL;
	int64_t stack_start;
		
	SL_TRACE_IN_HTM("[%d] sl_find_pure: start\n", (int)self->uniq_id);
	
	p_pred = p_skiplist->p_head;

	for (level = SKIPLIST_MAX_LEVEL-1; level >= 0; level--) {
		
		p_curr = p_pred->p_next[level];
		while (key > p_curr->key) {
			p_pred = p_curr;
			p_curr = p_pred->p_next[level];
		}
	
		if (l_found == -1 && key == p_curr->key) {
			l_found = level;
		}
		
		p_preds[level] = p_pred;
		p_succs[level] = p_curr;
	}
	
	SL_TRACE_IN_HTM("[%d] sl_find_pure: finish\n", (int)self->uniq_id);
	return l_found;
}

static int sl_find_hp(st_thread_t *self, 
							 skiplist_t *p_skiplist, int key, 
							 volatile sl_node_t **p_preds, volatile sl_node_t **p_succs, 
							 st_hp_record_t **hp_preds, st_hp_record_t **hp_succs)
{
	int level;
	int l_found = -1;
	volatile sl_node_t *p_pred = NULL;
	volatile sl_node_t *p_curr = NULL;
	
	st_hp_record_t *hp_pred = NULL;
	st_hp_record_t *hp_curr = NULL;
	st_hp_record_t *hp_temp = NULL;
			
	SL_TRACE("[%d] sl_find_slow_path: start\n", (int)self->uniq_id);
		
	hp_pred = ST_HP_alloc(self);
	hp_curr = ST_HP_alloc(self);
	
	ST_HP_INIT(self, hp_pred, &(p_skiplist->p_head));
	p_pred = p_skiplist->p_head;

	for (level = SKIPLIST_MAX_LEVEL-1; level >= 0; level--) {
		
		ST_HP_INIT(self, hp_curr, &(p_pred->p_next[level]));
		p_curr = p_pred->p_next[level];
		
		while (key > p_curr->key) {
			hp_temp = hp_pred;
			hp_pred = hp_curr;
			hp_curr = hp_temp;
			
			p_pred = p_curr;
			
			ST_HP_INIT(self, hp_curr, &(p_pred->p_next[level]));
			p_curr = p_pred->p_next[level];

		}
	
		if (l_found == -1 && key == p_curr->key) {
			l_found = level;
		}
		
		hp_preds[level] = hp_pred;
		p_preds[level] = (sl_node_t *)p_pred;
		
		hp_succs[level] = hp_curr;
		p_succs[level] = (sl_node_t *)p_curr;
		
		if ((level - 1) >= 0) {
			hp_pred = ST_HP_alloc(self);
			hp_curr = ST_HP_alloc(self);
		}
		
	}
		
	SL_TRACE("[%d] sl_find_slow_path: finish\n", (int)self->uniq_id);
	return l_found;
}

static int sl_find_stacktrack(st_thread_t *self, 
							   skiplist_t *p_skiplist, int key, 
							   volatile sl_node_t **p_preds, volatile sl_node_t **p_succs, 
							   st_hp_record_t **hp_preds, st_hp_record_t **hp_succs)
{
	int64_t stack_end;
	int level;
	int l_found = -1;
	volatile sl_node_t *p_pred = NULL;
	volatile sl_node_t *p_curr = NULL;
	int64_t stack_start;
	
	st_hp_record_t *hp_pred = NULL;
	st_hp_record_t *hp_curr = NULL;
	st_hp_record_t *hp_temp = NULL;
			
	SL_TRACE("[%d] sl_find_mixed: start\n", (int)self->uniq_id);
	
	ST_stack_add(self, &stack_start, &stack_end);
		
	hp_pred = ST_HP_alloc(self);
	hp_curr = ST_HP_alloc(self);
	
	ST_HP_INIT(self, hp_pred, &(p_skiplist->p_head));
	p_pred = p_skiplist->p_head;

	for (level = SKIPLIST_MAX_LEVEL-1; level >= 0; level--) {
		ST_SPLIT(self);
		
		ST_HP_INIT(self, hp_curr, &(p_pred->p_next[level]));
		p_curr = p_pred->p_next[level];
		
		while (key > p_curr->key) {
			ST_SPLIT(self);
			hp_temp = hp_pred;
			hp_pred = hp_curr;
			hp_curr = hp_temp;
			
			p_pred = p_curr;
			
			ST_HP_INIT(self, hp_curr, &(p_pred->p_next[level]));
			p_curr = p_pred->p_next[level];

		}
	
		if (l_found == -1 && key == p_curr->key) {
			ST_SPLIT(self);
			l_found = level;
		}
		
		hp_preds[level] = hp_pred;
		p_preds[level] = (sl_node_t *)p_pred;
		
		hp_succs[level] = hp_curr;
		p_succs[level] = (sl_node_t *)p_curr;
		
		if ((level - 1) >= 0) {
			ST_SPLIT(self);
			hp_pred = ST_HP_alloc(self);
			hp_curr = ST_HP_alloc(self);
		}
		
	}
	
	ST_stack_del(self);
		
	SL_TRACE("[%d] sl_find_mixed: finish\n", (int)self->uniq_id);
	return l_found;
}

/////////////////////////////////////////////////////////
// EXTERNAL FUNCTIONS
/////////////////////////////////////////////////////////

skiplist_t *skiplist_init() {
	int i;
	
	skiplist_t *p_skiplist = malloc(sizeof(skiplist_t));
	
	p_skiplist->p_head = sl_node_alloc();
	sl_node_init(NULL, p_skiplist->p_head, MIN_KEY, SKIPLIST_MAX_LEVEL-1);
	
	p_skiplist->p_tail = sl_node_alloc();
	sl_node_init(NULL, p_skiplist->p_tail, MAX_KEY, SKIPLIST_MAX_LEVEL-1);
	
	for (i = 0; i < SKIPLIST_MAX_LEVEL; i++) {
		p_skiplist->p_head->p_next[i] = p_skiplist->p_tail;
	}
	
	return p_skiplist;
}

int skiplist_contains_pure(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	int lFound;
	int ret;

	SL_TRACE("[%d] skiplist_contains_pure: start\n", (int)self->uniq_id);

	lFound = sl_find_pure(self, p_skiplist, key, p_preds, p_succs);
	ret = (lFound != -1) && (p_succs[lFound]->fullyLinked) && (!p_succs[lFound]->marked);
    
	SL_TRACE("[%d] skiplist_contains_pure: finish\n", (int)self->uniq_id);
	return ret;
}

int skiplist_contains_hp(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	st_hp_record_t *hp_preds[SKIPLIST_MAX_LEVEL] = {0,};
	st_hp_record_t *hp_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	int lFound;
	int ret;

	SL_TRACE("[%d] skiplist_contains_slow_path: start\n", (int)self->uniq_id);

	ST_init(self);
	
	lFound = sl_find_hp(self, p_skiplist, key, p_preds, p_succs, hp_preds, hp_succs);
	ret = (lFound != -1) && (p_succs[lFound]->fullyLinked) && (!p_succs[lFound]->marked);
    
	ST_finish(self);
	
	SL_TRACE("[%d] skiplist_contains_slow_path: finish\n", (int)self->uniq_id);
	return ret;
}

int skiplist_contains_stacktrack(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	int64_t stack_end;
	st_hp_record_t *hp_preds[SKIPLIST_MAX_LEVEL] = {0,};
	st_hp_record_t *hp_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	int lFound;
	int ret;
	int64_t stack_start;

	SL_TRACE("[%d] skiplist_contains_mixed: start\n", (int)self->uniq_id);

	ST_init(self);

	ST_stack_add(self, &stack_start, &stack_end);
	
	ST_split_start(self, OP_ID_CONTAINS);
	
	lFound = sl_find_stacktrack(self, p_skiplist, key, p_preds, p_succs, hp_preds, hp_succs);
	ret = (lFound != -1) && (p_succs[lFound]->fullyLinked) && (!p_succs[lFound]->marked);
    
	ST_split_finish(self);
	
	ST_finish(self);	
	
	SL_TRACE("[%d] skiplist_contains_mixed: finish\n", (int)self->uniq_id);
	return ret;
}

volatile sl_node_t *skiplist_insert_pure(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_node_found = NULL;
	volatile sl_node_t *p_pred = NULL;
	volatile sl_node_t *p_succ = NULL;
	volatile sl_node_t *p_new_node = NULL;
	volatile sl_node_t *ret = NULL;
	int level;
	int topLevel = -1;
	int lFound = -1;
	int done = 0;
			
	SL_TRACE("[%d] skiplist_insert_pure: start [ key = %d ]\n", (int)self->uniq_id, key);
	
	topLevel = sl_randomLevel(self->p_seed);
	
	while (!done) {		
		SL_TRACE_IN_HTM("[%d] skiplist_insert_pure: find\n", (int)self->uniq_id);
		
		lFound = sl_find_pure(self, p_skiplist, key, p_preds, p_succs);
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert_pure: find res=%d\n", (int)self->uniq_id, lFound);
		
		if (lFound != -1) {
			p_node_found = p_succs[lFound];
			if (!(p_node_found->marked)) {
				while (!(p_node_found->fullyLinked)) { CPU_RELAX; } // keep spinning
				return ret;
			}
			continue; // try again
		}

		int highestLocked = -1;
		
		int valid = 1;
		for (level = 0; valid && (level <= topLevel); level++)
		{	
			p_pred = p_preds[level];
			p_succ = p_succs[level];
			if (level == 0 || p_preds[level] != p_preds[level - 1]) { 
				// don't try to lock same node twice
				sl_node_lock(self, p_pred);
			}
			highestLocked = level;

			// make sure nothing has changed in between
			valid = !p_pred->marked && !p_succ->marked && p_pred->p_next[level] == p_succ;
		}
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert_pure: valid=%d\n", (int)self->uniq_id, valid);
		
		if (valid) {
			p_new_node = sl_node_alloc();
			sl_node_init(self, p_new_node, key, topLevel);
			ret = p_new_node;
			p_new_node->topLevel = topLevel;
			for (level = 0; level <= topLevel; level++) {
				p_new_node->p_next[level] = p_succs[level];
				p_preds[level]->p_next[level] = p_new_node;
			}
			p_new_node->fullyLinked = 1;
			done = 1;
		}

		// unlock everything here
		for (level = 0; level <= highestLocked; level++) {
			if (level == 0 || p_preds[level] != p_preds[level - 1]) {
				// don't try to unlock the same node twice
				sl_node_unlock(self, p_preds[level]);
			}
		}
	}
		
	SL_TRACE("[%d] skiplist_insert_pure: finish\n", (int)self->uniq_id);
	return ret;
}

volatile sl_node_t *skiplist_insert_hp(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	st_hp_record_t *hp_preds[SKIPLIST_MAX_LEVEL] = {0,};
	st_hp_record_t *hp_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_node_found = NULL;
	volatile sl_node_t *p_pred = NULL;
	volatile sl_node_t *p_succ = NULL;
	volatile sl_node_t *p_new_node = NULL;
	volatile sl_node_t *ret = NULL;
	int level;
	int topLevel;
	int lFound;
	int done = 0;
			
	SL_TRACE("[%d] skiplist_insert_slow_path: start\n", (int)self->uniq_id);
	
	topLevel = sl_randomLevel(self->p_seed);
	
	ST_init(self);

	while (!done) {
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert_slow_path: find\n", (int)self->uniq_id);
		
		lFound = sl_find_hp(self, p_skiplist, key, p_preds, p_succs, hp_preds, hp_succs);
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert_slow_path: find res=%d\n", (int)self->uniq_id, lFound);
		
		if (lFound != -1) {
			p_node_found = p_succs[lFound];
			if (!(p_node_found->marked)) {
				while (!(p_node_found->fullyLinked)) { CPU_RELAX; } // keep spinning
				return ret;
			}
			continue; // try again
		}

		int highestLocked = -1;
		
		int valid = 1;
		for (level = 0; valid && (level <= topLevel); level++)
		{	
			p_pred = p_preds[level];
			p_succ = p_succs[level];
			if (level == 0 || p_preds[level] != p_preds[level - 1]) {
				// don't try to lock same node twice
				sl_node_lock(self, p_pred);
			}
			highestLocked = level;

			// make sure nothing has changed in between
			valid = !p_pred->marked && !p_succ->marked && p_pred->p_next[level] == p_succ;
		}
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert_slow_path: valid=%d\n", (int)self->uniq_id, valid);
		
		if (valid) {
			p_new_node = sl_node_alloc();
			sl_node_init(self, p_new_node, key, topLevel);
			ret = p_new_node;
			p_new_node->topLevel = topLevel;
			for (level = 0; level <= topLevel; level++) {
				p_new_node->p_next[level] = p_succs[level];
				p_preds[level]->p_next[level] = p_new_node;
			}
			p_new_node->fullyLinked = 1;
			done = 1;
		}

		// unlock everything here
		for (level = 0; level <= highestLocked; level++) {
			if (level == 0 || p_preds[level] != p_preds[level - 1]) { 
				// don't try to unlock the same node twice
				sl_node_unlock(self, p_preds[level]);
			}
		}
	}
	
	ST_finish(self);
	
	SL_TRACE("[%d] skiplist_insert_slow_path: finish\n", (int)self->uniq_id);
	return ret;
}

volatile sl_node_t *skiplist_insert_stacktrack(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	int64_t stack_end;
	st_hp_record_t *hp_preds[SKIPLIST_MAX_LEVEL] = {0,};
	st_hp_record_t *hp_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_node_found = NULL;
	volatile sl_node_t *p_pred = NULL;
	volatile sl_node_t *p_succ = NULL;
	volatile sl_node_t *p_new_node = NULL;
	volatile sl_node_t *ret = NULL;
	int level;
	int topLevel = -1;
	int lFound = -1;
	int done = 0;
	int64_t stack_start;
			
	SL_TRACE("[%d] skiplist_insert: start [ key = %d ]\n", (int)self->uniq_id, key);
	
	ST_init(self);
	
	ST_stack_add(self, &stack_start, &stack_end);
	
	topLevel = sl_randomLevel(self->p_seed);
	
	ST_split_start(self, OP_ID_INSERT);
	
	while (!done) {
		ST_SPLIT(self);
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert: find\n", (int)self->uniq_id);
		
		lFound = sl_find_stacktrack(self, p_skiplist, key, p_preds, p_succs, hp_preds, hp_succs);
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert: find res=%d\n", (int)self->uniq_id, lFound);
		
		if (lFound != -1) {
			ST_SPLIT(self);
			p_node_found = p_succs[lFound];
			if (!(p_node_found->marked)) {
				ST_SPLIT(self);
				while (!(p_node_found->fullyLinked)) { CPU_RELAX; } // keep spinning
				ST_split_finish(self);
				ST_finish(self);
				return ret;
			}
			continue; // try again
		}

		int highestLocked = -1;
		
		int valid = 1;
		for (level = 0; valid && (level <= topLevel); level++)
		{
			ST_SPLIT(self);
			
			p_pred = p_preds[level];
			p_succ = p_succs[level];
			if (level == 0 || p_preds[level] != p_preds[level - 1]) { 
				ST_SPLIT(self);
				// don't try to lock same node twice
				sl_node_lock(self, p_pred);
			}
			highestLocked = level;

			// make sure nothing has changed in between
			valid = !p_pred->marked && !p_succ->marked && p_pred->p_next[level] == p_succ;
		}
		
		SL_TRACE_IN_HTM("[%d] skiplist_insert: valid=%d\n", (int)self->uniq_id, valid);
		
		if (valid) {
			ST_SPLIT(self);
			p_new_node = sl_node_alloc();
			sl_node_init(self, p_new_node, key, topLevel);
			ret = p_new_node;
			p_new_node->topLevel = topLevel;
			for (level = 0; level <= topLevel; level++) {
				ST_SPLIT(self);
				p_new_node->p_next[level] = p_succs[level];
				p_preds[level]->p_next[level] = p_new_node;
			}
			p_new_node->fullyLinked = 1;
			done = 1;
		}

		// unlock everything here
		for (level = 0; level <= highestLocked; level++) {
			ST_SPLIT(self);
			if (level == 0 || p_preds[level] != p_preds[level - 1]) { 
				ST_SPLIT(self);
				// don't try to unlock the same node twice
				sl_node_unlock(self, p_preds[level]);
			}
		}
	}
	
	ST_split_finish(self);
	ST_finish(self);
	
	SL_TRACE("[%d] skiplist_insert: finish\n", (int)self->uniq_id);
	return ret;
}

int skiplist_remove_pure(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_victim = NULL;
	volatile sl_node_t *p_pred = NULL;
	int i;
	int level;
	int lFound;
	int highestLocked;
	int valid;
	int isMarked = 0;
	int topLevel = -1;
	int ret = 0;

	SL_TRACE("[%d] skiplist_remove_pure: start [ key = %d ]\n", (int)self->uniq_id, key);
		
	while (1) {
		
		lFound = sl_find_pure(self, p_skiplist, key, p_preds, p_succs);
		
		if (lFound == -1) {
			break;
		}
		
		SL_TRACE_IN_HTM("[%d] skiplist_remove_pure: find res = %d\n", (int)self->uniq_id, lFound);
		p_victim = p_succs[lFound];
		
		if ((!isMarked) ||
			(p_victim->fullyLinked && p_victim->topLevel == lFound && !p_victim->marked)) 
		{
			if (!isMarked) {
				topLevel = p_victim->topLevel;
				sl_node_lock(self, p_victim);
				if (p_victim->marked) {
					sl_node_unlock(self, p_victim);
					ret = 0;
					break;
				}
				
				p_victim->marked = 1;
				isMarked = 1;
				
			}
			
			highestLocked = -1;
			valid = 1;

			for (level = 0; valid && (level <= topLevel); level++)
			{
				p_pred = p_preds[level];
				if (level == 0 || p_preds[level] != p_preds[level - 1]) { // don't do twice
					sl_node_lock(self, p_pred);
				}
				highestLocked = level;
				valid = !p_pred->marked && p_pred->p_next[level] == p_victim;
				if (!valid) {
					SL_TRACE_IN_HTM("[%d] skiplist_remove_pure: not valid [p_pred->marked = %d]\n", (int)self->uniq_id, p_pred->marked);
				}
			}
			
			
			
			if (valid) {
				for (level = topLevel; level >= 0; level--) {
					p_preds[level]->p_next[level] = p_victim->p_next[level];
				}
				sl_node_unlock(self, p_victim);
				ret = 1;
				
			} else {
				p_victim->marked = 0;
				isMarked = 0;
				sl_node_unlock(self, p_victim);
				
			}
			
			// unlock mutexes
			for (i = 0; i <= highestLocked; i++) {
				if (i == 0 || p_preds[i] != p_preds[i - 1]) {
					sl_node_unlock(self, p_preds[i]);
				}
			}
			
			if (valid) {
				break;
			}
		}

	}

	SL_TRACE("[%d] skiplist_remove_pure: finish\n", (int)self->uniq_id);
	return ret;
}

int skiplist_remove_hp(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	st_hp_record_t *hp_preds[SKIPLIST_MAX_LEVEL] = {0,};
	st_hp_record_t *hp_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_victim = NULL;
	volatile sl_node_t *p_pred = NULL;
	int i;
	int level;
	int lFound;
	int highestLocked;
	int valid;
	int isMarked = 0;
	int topLevel = -1;
	int ret = 0;

	SL_TRACE("[%d] skiplist_remove_slow_path: start\n", (int)self->uniq_id);
	
	ST_init(self);
	
	while (1) {
		
		lFound = sl_find_hp(self, p_skiplist, key, p_preds, p_succs, hp_preds, hp_succs);
		
		if (lFound == -1) {
			break;
		}
		
		p_victim = p_succs[lFound];
		
		if ((!isMarked) ||
			(p_victim->fullyLinked && p_victim->topLevel == lFound && !p_victim->marked)) 
		{
			if (!isMarked) {
				topLevel = p_victim->topLevel;
				sl_node_lock(self, p_victim);
				if (p_victim->marked) {
					sl_node_unlock(self, p_victim);
					ret = 0;
					break;
				}
				
				p_victim->marked = 1;
				isMarked = 1;
				
			}
			
			highestLocked = -1;
			valid = 1;

			for (level = 0; valid && (level <= topLevel); level++)
			{
				p_pred = p_preds[level];
				if (level == 0 || p_preds[level] != p_preds[level - 1]) { // don't do twice
					sl_node_lock(self, p_pred);
				}
				highestLocked = level;
				valid = !p_pred->marked && p_pred->p_next[level] == p_victim;
			}

			if (valid) {
				for (level = topLevel; level >= 0; level--) {
					p_preds[level]->p_next[level] = p_victim->p_next[level];
				}
				sl_node_unlock(self, p_victim);
				ret = 1;
			} else {
				p_victim->marked = 0;
				isMarked = 0;
				sl_node_unlock(self, p_victim);
			}
			
			// unlock mutexes
			for (i = 0; i <= highestLocked; i++) {
				if (i == 0 || p_preds[i] != p_preds[i - 1]) {
					sl_node_unlock(self, p_preds[i]);
				}
			}
			
			if (valid) {
				break;
			}
		}

	}

	ST_finish(self);
	
	if (ret == 1) {
		ST_free(self, (int64_t *)p_victim);
	}

	SL_TRACE("[%d] SL_remove_slow_path: finish\n", (int)self->uniq_id);
	return ret;
}

int skiplist_remove_stacktrack(st_thread_t *self, skiplist_t *p_skiplist, int key) {
	int64_t stack_end;
	st_hp_record_t *hp_preds[SKIPLIST_MAX_LEVEL] = {0,};
	st_hp_record_t *hp_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_preds[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_succs[SKIPLIST_MAX_LEVEL] = {0,};
	volatile sl_node_t *p_victim = NULL;
	volatile sl_node_t *p_pred = NULL;
	int i;
	int level;
	int lFound;
	int highestLocked;
	int valid;
	int isMarked = 0;
	int topLevel = -1;
	int ret = 0;
	int64_t stack_start;

	SL_TRACE("[%d] skiplist_remove: start [ key = %d ]\n", (int)self->uniq_id, key);
	
	ST_init(self);
	
	ST_stack_add(self, &stack_start, &stack_end);
	
	ST_split_start(self, OP_ID_REMOVE);
	
	while (1) {
		ST_SPLIT(self);
		
		lFound = sl_find_stacktrack(self, p_skiplist, key, p_preds, p_succs, hp_preds, hp_succs);
		
		if (lFound == -1) {
			ST_SPLIT(self);
			break;
		}
		
		SL_TRACE_IN_HTM("[%d] skiplist_remove: find res = %d\n", (int)self->uniq_id, lFound);
		p_victim = p_succs[lFound];
		
		if ((!isMarked) ||
			(p_victim->fullyLinked && p_victim->topLevel == lFound && !p_victim->marked)) 
		{
			ST_SPLIT(self);
			if (!isMarked) {
				ST_SPLIT(self);
				topLevel = p_victim->topLevel;
				sl_node_lock(self, p_victim);
				if (p_victim->marked) {
					ST_SPLIT(self);
					sl_node_unlock(self, p_victim);
					ret = 0;
					break;
				}
				
				p_victim->marked = 1;
				isMarked = 1;
				
			}
			
			highestLocked = -1;
			valid = 1;

			for (level = 0; valid && (level <= topLevel); level++)
			{
				ST_SPLIT(self);
				p_pred = p_preds[level];
				if (level == 0 || p_preds[level] != p_preds[level - 1]) { // don't do twice
					ST_SPLIT(self);
					sl_node_lock(self, p_pred);
				}
				highestLocked = level;
				valid = !p_pred->marked && p_pred->p_next[level] == p_victim;
				if (!valid) {
					SL_TRACE_IN_HTM("[%d] skiplist_remove: not valid [p_pred->marked = %d]\n", (int)self->uniq_id, p_pred->marked);
				}
			}
			
			if (valid) {
				ST_SPLIT(self);
				for (level = topLevel; level >= 0; level--) {
					ST_SPLIT(self);
					p_preds[level]->p_next[level] = p_victim->p_next[level];
				}
				sl_node_unlock(self, p_victim);
				ret = 1;
				
			} else {
				ST_SPLIT(self);
				p_victim->marked = 0;
				isMarked = 0;
				sl_node_unlock(self, p_victim);
				
			}
			
			// unlock mutexes
			for (i = 0; i <= highestLocked; i++) {
				ST_SPLIT(self);
				if (i == 0 || p_preds[i] != p_preds[i - 1]) {
					ST_SPLIT(self);
					sl_node_unlock(self, p_preds[i]);
				}
			}
			
			if (valid) {
				ST_SPLIT(self);
				break;
			}
		}

	}

	ST_split_finish(self);
	ST_finish(self);
	
	if (ret == 1) {
		SL_TRACE("[%d] skiplist_remove: ST_FREE p_victim = %p\n", (int)self->uniq_id, p_victim);
		ST_free(self, (int64_t *)p_victim);
	}

	SL_TRACE("[%d] skiplist_remove: finish\n", (int)self->uniq_id);
	return ret;
}

int skiplist_size(skiplist_t *p_skiplist) {
	int n_nodes;
	volatile sl_node_t *p_node;
	
	n_nodes = 0;
	p_node = p_skiplist->p_head;
	
	while (p_node->p_next[0] != NULL) {
		n_nodes++;
		p_node = p_node->p_next[0];
	}
	n_nodes -= 1;

	return n_nodes;
}

void skiplist_print_stats(skiplist_t *p_skiplist) {
	int level;
	int n_nodes;
	volatile sl_node_t *p_node;
	
	printf("-------------------------------------------------\n");
	printf("  Skip-List status:\n");

	for (level = SKIPLIST_MAX_LEVEL-1; level >= 0; level--)
	{
		n_nodes = 0;
		p_node = p_skiplist->p_head;
		
		while (p_node->p_next[level] != NULL) {
			n_nodes++;
			p_node = p_node->p_next[level];
		}
		n_nodes -= 1;

		printf("    nodes on level[%d] = %d\n", level, n_nodes);
		
	}
	
	printf("-------------------------------------------------\n");
	
	HTM_print_stats();
	
	ST_print_stats();
	

}