// rwlock.cpp -- Part A driver (iii): std::shared_mutex, readers share the lock,
// the writer takes it exclusively (store variant nova_kv_rwlock).
#include "driver_common.h"

int main(int argc, char** argv) { return run_part_a_driver("rwlock", 0, argc, argv); }
