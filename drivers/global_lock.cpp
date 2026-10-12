// global_lock.cpp -- Part A driver (ii): one std::mutex held for every
// put / get / erase (store variant nova_kv_global_lock).
#include "driver_common.h"

int main(int argc, char** argv) { return run_driver("global_lock", 0, argc, argv); }
