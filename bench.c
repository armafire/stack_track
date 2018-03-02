
#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "common.h"
#include "atomics.h"
#include "skip-list.h"

///////////////////////////////////////////////////////////////////////////////
// CONFIGURATION
///////////////////////////////////////////////////////////////////////////////
#define ALG_TYPE_PURE                   (0)
#define ALG_TYPE_HAZARD_POINTERS        (1)
#define ALG_TYPE_STACK_TRACK            (2)
#define ALG_TYPE_FORKSCAN               (3)

#define DEFAULT_ALG_TYPE			    (ALG_TYPE_PURE)
#define DEFAULT_MAX_SEGMENT_LEN         (50)
#define DEFAULT_MAX_FREE_LIST           (100)
#define DEFAULT_SLOW_PATH_PROB          (0)
#define DEFAULT_DURATION                (10000)
#define DEFAULT_INITIAL                 (256)
#define DEFAULT_NB_THREADS              (1)
#define DEFAULT_RANGE                   (DEFAULT_INITIAL * 2)
#define DEFAULT_SEED                    (0)
#define DEFAULT_UPDATE                  (20)

#define XSTR(s)                         STR(s)
#define STR(s)                          #s

///////////////////////////////////////////////////////////////////////////////
// GLOBALS
///////////////////////////////////////////////////////////////////////////////

static volatile long padding[50];

static volatile int stop;

///////////////////////////////////////////////////////////////////////////////
// FUNCTIONS
///////////////////////////////////////////////////////////////////////////////
static inline void rand_init(int *p_seed)
{
	*p_seed = rand();
}

static inline int rand_range(int n, int *p_seed)
{    
	int v = MY_RAND(p_seed) % n;
	return v;
}

typedef struct thread_data {
	long uniq_id;
	struct barrier *barrier;
	int initial;
	
	unsigned long nb_add;
	unsigned long nb_remove;
	unsigned long nb_contains;
	unsigned long nb_found;
	int alg_type;
	int max_segment_len;
	int max_free_list;
	
	int *p_seed;
	int seed;
	
	skiplist_t *p_set;
	
	int diff;
	int range;
	int update;
	int alternate;
	
	st_thread_t *p_st;
	st_thread_t st;
	
	char padding[64];
	
} thread_data_t;


///////////////////////////////////////////////////////////////////////////////
// BARRIER
///////////////////////////////////////////////////////////////////////////////
 
typedef struct barrier {
  pthread_cond_t complete;
  pthread_mutex_t mutex;
  int count;
  int crossing;
} barrier_t;

static void barrier_init(barrier_t *b, int n)
{
  pthread_cond_init(&b->complete, NULL);
  pthread_mutex_init(&b->mutex, NULL);
  b->count = n;
  b->crossing = 0;
}

static void barrier_cross(barrier_t *b)
{
  pthread_mutex_lock(&b->mutex);
  /* One more thread through */
  b->crossing++;
  /* If not all here, wait */
  if (b->crossing < b->count) {
    pthread_cond_wait(&b->complete, &b->mutex);
  } else {
    pthread_cond_broadcast(&b->complete);
    /* Reset for next time */
    b->crossing = 0;
  }
  pthread_mutex_unlock(&b->mutex);
}

/////////////////////////////////////////////////////////
// SKIP-LIST
/////////////////////////////////////////////////////////
int set_contains(thread_data_t *p_td, int key) {
	int res;

	if (p_td->alg_type == ALG_TYPE_PURE) {
		res = skiplist_contains_pure(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_HAZARD_POINTERS) {
		res = skiplist_contains_hp(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_STACK_TRACK) {
		res = skiplist_contains_stacktrack(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_FORKSCAN) {
		res = skiplist_contains_forkscan(p_td->p_st, p_td->p_set, key);
	}

	return res;
}

int set_add(thread_data_t *p_td, int key) {
	volatile sl_node_t *p_node;
	
	if (p_td->alg_type == ALG_TYPE_PURE) {
		p_node = skiplist_insert_pure(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_HAZARD_POINTERS) {
		p_node = skiplist_insert_hp(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_STACK_TRACK) {
		p_node = skiplist_insert_stacktrack(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_FORKSCAN) {
		p_node = skiplist_insert_forkscan(p_td->p_st, p_td->p_set, key);
	}

	if (p_node != NULL) {
		return 1;
	} 

	return 0;	
}

int set_remove(thread_data_t *p_td, int key) {
	int res;

	if (p_td->alg_type == ALG_TYPE_PURE) {
		res = skiplist_remove_pure(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_HAZARD_POINTERS) {
		res = skiplist_remove_hp(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_STACK_TRACK) {
		res = skiplist_remove_stacktrack(p_td->p_st, p_td->p_set, key);
	} else if (p_td->alg_type == ALG_TYPE_FORKSCAN) {
		res = skiplist_remove_forkscan(p_td->p_st, p_td->p_set, key);
	}

	return res;
}

/////////////////////////////////////////////////////////
// STRESS TEST
/////////////////////////////////////////////////////////
static void *test(void *p_arg)
{
	int i;
	int op;
	int key;
	int last = -1;
	thread_data_t *p_td = (thread_data_t *)p_arg;

	ST_thread_init(p_td->p_st, p_td->p_seed, p_td->max_segment_len, p_td->max_free_list);

	if (p_td->p_st->uniq_id == 0) {
		/* Populate set */
		printf("[%ld] Init: adding %d entries to set.\n", p_td->uniq_id, p_td->initial);
		i = 0;
		while (i < p_td->initial) {
			key = rand_range(p_td->range, p_td->p_seed) + 1;
			if (set_add(p_td, key)) {
				i++;
			}
		}
		printf("[%ld] Init: done.\n", p_td->uniq_id);
	}
	
	/* Wait on barrier */
	barrier_cross(p_td->barrier);

	while (stop == 0) {
		
		op = rand_range(100, p_td->p_seed);
		
		if (op < p_td->update) {
			if (p_td->alternate) {
				/* Alternate insertions and removals */
				if (last < 0) {
					/* Add random value */
					key = rand_range(p_td->range, p_td->p_seed) + 1;
					if (set_add(p_td, key)) {
						p_td->diff++;
						last = key;
					}
					p_td->nb_add++;
				} else {
					/* Remove last value */
					if (set_remove(p_td, last)) {
						p_td->diff--;
					}
					p_td->nb_remove++;
					last = -1;
				}
			} else {
				/* Randomly perform insertions and removals */
				key = rand_range(p_td->range, p_td->p_seed) + 1;
				if ((op & 0x01) == 0) {
					/* Add random value */
					if (set_add(p_td, key)) {
						p_td->diff++;
					}
					p_td->nb_add++;
				} else {
					/* Remove random value */
					if (set_remove(p_td, key)) {
						p_td->diff--;
					}
					p_td->nb_remove++;
				}
			}
		} else {
			/* Look for random value */
			key = rand_range(p_td->range, p_td->p_seed) + 1;
			if (set_contains(p_td, key)) {
				p_td->nb_found++;
			}
			p_td->nb_contains++;
		}
	}

	ST_thread_finish(p_td->p_st);

	return NULL;
}

int main(int argc, char **argv)
{
	struct option long_options[] = {
			// These options don't set a flag
			{"help",                      no_argument,       NULL, 'h'},
			{"do-not-alternate",          no_argument,       NULL, 'a'},
			{"duration",                  required_argument, NULL, 'd'},
			{"initial-size",              required_argument, NULL, 'i'},
			{"num-threads",               required_argument, NULL, 'n'},
			{"range",                     required_argument, NULL, 'r'},
			{"seed",                      required_argument, NULL, 's'},
			{"update-rate",               required_argument, NULL, 'u'},
			{"max-segment-length",        required_argument, NULL, 'l'},
			{"free-batch-size",           required_argument, NULL, 'f'},
			{"alg_type",                  required_argument, NULL, 'p'},
			{NULL, 0, NULL, 0}
	};

	skiplist_t *p_set;
	int i, c, val, cur_size, size, ret;
	unsigned long reads, updates;
	thread_data_t *data;
	pthread_t *threads;
	pthread_attr_t attr;
	barrier_t barrier;
	struct timeval start, end;
	struct timespec timeout, remaining;
	int alg_type = DEFAULT_ALG_TYPE;
	int max_segment_len = DEFAULT_MAX_SEGMENT_LEN;
	int max_free_list = DEFAULT_MAX_FREE_LIST;
	int duration = DEFAULT_DURATION;
	int initial = DEFAULT_INITIAL;
	int nb_threads = DEFAULT_NB_THREADS;
	int range = DEFAULT_RANGE;
	int seed = DEFAULT_SEED;
	int update = DEFAULT_UPDATE;
	int alternate = 1;
	sigset_t block_set;

	while(1) {
		i = 0;
		c = getopt_long(argc, argv, "had:i:n:r:s:u:l:f:p:", long_options, &i);

		if(c == -1)
			break;

		if(c == 0 && long_options[i].flag == 0)
			c = long_options[i].val;

		switch(c) {
			case 0:
			/* Flag is automatically set */
				break;
			case 'h':
				printf("bench "
					"()\n"
					"\n"
					"Usage:\n"
					"  bench [options...]\n"
					"\n"
					"Options:\n"
					"  -h, --help\n"
					"        Print this message\n"
					"  -p, --protocol-type\n"
					"        0 - Pure: no memory reclamation\n"
					"        1 - Hazard Pointers\n"
					"        2 - Stack Track\n"
					"        3 - Forkscan\n"
					"  -l, --max-segment-length\n"
					"        Maximum segment length\n"
					"  -f, --free-batch-size\n"
					"        Number of free operations till actual deallocation\n"
					"  -a, --do-not-alternate\n"
					"        Do not alternate insertions and removals\n"
					"  -d, --duration <int>\n"
					"        Test duration in milliseconds (0=infinite, default=" XSTR(DEFAULT_DURATION) ")\n"
					"  -i, --initial-size <int>\n"
					"        Number of elements to insert before test (default=" XSTR(DEFAULT_INITIAL) ")\n"
					"  -n, --num-threads <int>\n"
					"        Number of threads (default=" XSTR(DEFAULT_NB_THREADS) ")\n"
					"  -r, --range <int>\n"
					"        Range of integer values inserted in set (default=" XSTR(DEFAULT_RANGE) ")\n"
					"  -s, --seed <int>\n"
					"        RNG seed (0=time-based, default=" XSTR(DEFAULT_SEED) ")\n"
					"  -u, --update-rate <int>\n"
					"        Percentage of update transactions (default=" XSTR(DEFAULT_UPDATE) ")\n"
					);
				exit(0);
			case 'l':
				max_segment_len = atoi(optarg);
				break;
			case 'p':
				alg_type = atoi(optarg);
				if ((alg_type != ALG_TYPE_PURE) && 
				    (alg_type != ALG_TYPE_HAZARD_POINTERS) &&
				    (alg_type != ALG_TYPE_STACK_TRACK) &&
				    (alg_type != ALG_TYPE_FORKSCAN)) {
					printf("ERROR: protocol type must be 0 (pure) or 1 (hazard pointers) or 2 (stack track) or 3 (forkscan).\n");
					exit(1);
				}
				break;	
			case 'f':
				max_free_list = atoi(optarg);
				if (max_free_list > ST_MAX_FREE_LIST) {
					printf("ERROR: free batch size must be less than %d\n", ST_MAX_FREE_LIST);
					exit(1);	
				}
				break;
			case 'a':
				alternate = 0;
				break;
			case 'd':
				duration = atoi(optarg);
				break;
			case 'i':
				initial = atoi(optarg);
				break;
			case 'n':
				nb_threads = atoi(optarg);
				break;
			case 'r':
				range = atoi(optarg);
				break;
			case 's':
				seed = atoi(optarg);
				break;
			case 'u':
				update = atoi(optarg);
				break;
			case '?':
				printf("Use -h or --help for help\n");
				exit(0);
			default:
				exit(1);
		}
	}

	assert(duration >= 0);
	assert(initial >= 0);
	assert(nb_threads > 0);
	assert(range > 0 && range >= initial);
	assert(update >= 0 && update <= 100);

	if (alg_type == ALG_TYPE_PURE) {
		printf("Set type           : skip-list [** pure **]\n");
	} else if (alg_type == ALG_TYPE_HAZARD_POINTERS) {
		printf("Set type           : skip-list [** hazard pointers **]\n");
	} else if (alg_type == ALG_TYPE_STACK_TRACK) {
		printf("Set type           : skip-list [** stack-track **]\n");
	} else if (alg_type == ALG_TYPE_FORKSCAN) {
		printf("Set type           : skip-list [** forkscan **]\n");
	} else {
		abort();
	}
	printf("Max segment length : %d\n", max_segment_len);
	printf("Max free list      : %d\n", max_free_list);
	printf("Duration           : %d\n", duration);
	printf("Initial size       : %d\n", initial);
	printf("Nb threads         : %d\n", nb_threads);
	printf("Value range        : %d\n", range);
	printf("Seed               : %d\n", seed);
	printf("Update rate        : %d\n", update);
	printf("Alternate          : %d\n", alternate);
	printf("Type sizes         : int=%d/long=%d/ptr=%d/word=%d\n",
		(int)sizeof(int),
		(int)sizeof(long),
		(int)sizeof(void *),
		(int)sizeof(size_t));

	timeout.tv_sec = duration / 1000;
	timeout.tv_nsec = (duration % 1000) * 1000000;

	if ((data = (thread_data_t *)malloc(nb_threads * sizeof(thread_data_t))) == NULL) {
		perror("malloc");
		exit(1);
	}

	memset(data, 0, nb_threads * sizeof(thread_data_t));

	if ((threads = (pthread_t *)malloc(nb_threads * sizeof(pthread_t))) == NULL) {
		perror("malloc");
		exit(1);
	}

	if (seed == 0) {
		srand((int)time(NULL));
	} else {
		srand(seed);
	}
	
	p_set = skiplist_init();
	
	stop = 0;

	if (alternate == 0 && range != initial * 2) {
		printf("WARNING: range is not twice the initial set size\n");
	}

	size = initial;
	printf("Set size           : %d\n", size);
	
	/* Access set from all threads */
	barrier_init(&barrier, nb_threads + 1);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	for (i = 0; i < nb_threads; i++) {
		printf("Creating thread %d\n", i);

		data[i].uniq_id = i;
		data[i].range = range;
		data[i].update = update;
		data[i].alternate = alternate;
		data[i].nb_add = 0;
		data[i].nb_remove = 0;
		data[i].nb_contains = 0;
		data[i].nb_found = 0;
		data[i].diff = 0;
		data[i].p_seed = &(data[i].seed);
		rand_init(data[i].p_seed);
		data[i].p_set = p_set;
		data[i].barrier = &barrier;
		data[i].initial = initial;
		data[i].alg_type = alg_type;
		data[i].max_segment_len = max_segment_len;
		data[i].max_free_list = max_free_list;
		data[i].p_st = &(data[i].st);  

		if (pthread_create(&threads[i], &attr, test, (void *)(&data[i])) != 0) {
			fprintf(stderr, "Error creating thread\n");
			exit(1);
		}
	}
	pthread_attr_destroy(&attr);

	/* Start threads */
	barrier_cross(&barrier);

	printf("STARTING...\n");
	gettimeofday(&start, NULL);
	if (duration > 0) {
		while (-1 == nanosleep(&timeout, &remaining)) {
			timeout = remaining;
		}
	} else {
		sigemptyset(&block_set);
		sigsuspend(&block_set);
	}
	stop = 1;
	gettimeofday(&end, NULL);
	printf("STOPPING...\n");

	/* Wait for thread completion */
	for (i = 0; i < nb_threads; i++) {
		if (pthread_join(threads[i], NULL) != 0) {
			fprintf(stderr, "Error waiting for thread completion\n");
			exit(1);
		}
	}

	duration = (end.tv_sec * 1000 + end.tv_usec / 1000) - (start.tv_sec * 1000 + start.tv_usec / 1000);
	reads = 0;
	updates = 0;
	for (i = 0; i < nb_threads; i++) {
		printf("Thread %d\n", i);
		printf("  #add        : %lu\n", data[i].nb_add);
		printf("  #remove     : %lu\n", data[i].nb_remove);
		printf("  #contains   : %lu\n", data[i].nb_contains);
		printf("  #found      : %lu\n", data[i].nb_found);
		reads += data[i].nb_contains;
		updates += (data[i].nb_add + data[i].nb_remove);
		size += data[i].diff;
	}
	cur_size = skiplist_size(p_set); 
	printf("Set size       : %d (expected: %d)\n", cur_size, size);
	printf("Duration       : %d (ms)\n", duration);
	printf("#ops           : %lu (%f / s)\n", reads + updates, (reads + updates) * 1000.0 / duration);
	printf("#read ops      : %lu (%f / s)\n", reads, reads * 1000.0 / duration);
	printf("#update ops    : %lu (%f / s)\n", updates, updates * 1000.0 / duration);

	printf("\n");
	skiplist_print_stats(p_set);
	printf("\n");

	if (cur_size != size) {
		printf("----------------------------\n");
		printf("WARNING: The set size [%d] is not as expected [%d]\n", cur_size, size);
		printf("----------------------------\n");
	}
	
	/* Delete set */
	//

	/* Cleanup */
	//

	free(threads);
	free(data);

	return ret;
}
