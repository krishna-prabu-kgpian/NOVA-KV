// rcu.cpp -- Part B driver: the supplied workload generator against the RCU
// store (store variant nova_kv_rcu): readers take no lock and load the current
// version once per get.
//
// MILESTONE B1: replaced versions are not reclaimed yet, so loading the default
// 100,000 trades retains every intermediate version (several GiB) until the
// store is destroyed. Pass smaller m / n to bound it:
//   rcu [num_readers] [duration_sec] [m] [n]
#include "driver_common.h"

int main(int argc, char** argv) { return run_driver("rcu", 0, argc, argv); }
