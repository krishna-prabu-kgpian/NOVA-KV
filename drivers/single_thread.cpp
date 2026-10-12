// single_thread.cpp -- Part A driver (i): no concurrency control in the store.
//
// The store variant linked here has no lock at all, so the workload runs with
// exactly one reader thread, after the generator's writer has finished: at no
// time do two threads use the store. More reader threads on this variant would
// not be a valid single-threaded baseline, so the reader count is fixed at 1.
#include "driver_common.h"

int main(int argc, char** argv) { return run_part_a_driver("single", 1, argc, argv); }
