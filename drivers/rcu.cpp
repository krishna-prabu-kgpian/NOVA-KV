// rcu.cpp -- Part B driver: the supplied workload generator against the RCU
// store (store variant nova_kv_rcu). Readers take no lock: each get announces
// an epoch, loads the current version once and copies the value out. Replaced
// versions are reclaimed in the background by epoch-based reclamation.
//
//   rcu [num_readers] [duration_sec] [m] [n]
#include "driver_common.h"

int main(int argc, char** argv) { return run_driver("rcu", 0, argc, argv); }
