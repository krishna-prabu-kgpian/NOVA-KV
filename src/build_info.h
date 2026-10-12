// build_info.h -- internal (non-assignment) introspection used by tests and
// benchmark harnesses to record which store variant a binary was linked with.
#pragma once

// Name of the synchronization policy compiled into the linked store library:
// "single", "global_lock" or "rwlock".
const char* nova_kv_sync_mode_name() noexcept;
