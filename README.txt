
STACK-TRACK 1.0 (12/19/2014) 
============================

* General
---------
This is a C implementation of the lock-free stack-track memory reclamation scheme for the concurrent skip-list.

The stack-track scheme is described in: http://people.csail.mit.edu/amatveev/StackTrack_EuroSys2014.pdf .

This implementation allows to compare the stack-track scheme to the pure skip-list, that has no memory reclamation at all (leaks memory), and to the hazard pointers skip-list, that uses hazard pointers to protect shared references (by M.Michael http://dl.acm.org/citation.cfm?id=987595).

* Code Maintainer
-----------------
Name:  Alexander Matveev
Email: amatveev@csail.mit.edu

* Compilation
-------------
Requires Forkscan to be installed (follow the build, install procedure):
https://github.com/Willtor/forkscan

Execute "make"

* Execution Options
-------------------
  -h, --help
        Print this message
  -p, --protocol-type
        0 - Pure: no memory reclamation
        1 - Hazard Pointers
        2 - Stack Track
        3 - Forkscan
        (default=(0))
  -l, --max-segment-length
        Maximum segment length (default=(50))
  -f, --free-batch-size
        Number of free operations till actual deallocation (default=(1000))
  -a, --do-not-alternate
        Do not alternate insertions and removals
  -d, --duration <int>
        Test duration in milliseconds (0=infinite, default=(10000))
  -i, --initial-size <int>
        Number of elements to insert before test (default=(256))
  -n, --num-threads <int>
        Number of threads (default=(1))
  -r, --range <int>
        Range of integer values inserted in set (default=((256) * 2))
  -s, --seed <int>
        RNG seed (0=time-based, default=(0))
  -u, --update-rate <int>
        Percentage of update transactions (default=(20))

* Example
---------
./bench-skiplist -a -u20 -i100000 -r200000 -f1000 -l20 -d10000 -p2 -n16

==> Initializes a 100,000 nodes skip-list with stack-track memory reclamation, and then executes 16 threads for 10 seconds. 
==> The ratio of insert/remove is 20% and there is no alternation (completely randomized insert/remove)
==> The stack-track initial segment length is 20, and the amount of deallocations till actual reclamation (stacks' scan) is 1000.

* Recommendations
-----------------
1. The malloc/free library should be HTM friendly. A good example is "tc-malloc" from Google Perf Tools library (https://code.google.com/p/gperftools/)

2. To bind threads to specific cores use the "taskset" linux command. For example, on 8-core Intel Haswell processor, the command "taskset -c 0-7 prog arg1 arg2 ..." allows to avoid the negative HyperThreading HTM effects for 8-thread executions; It binds each thread to another core, so no 2 threads execute on the same core.
