#pragma once

#include "common.h"

// Vectored exception handler (first-chance log) and unhandled-exception filter (crash report).
void install_crash_handlers();
struct BuildProfile;
bool install_rdtsc_emulation(const BuildProfile &profile); // rdtsc sites trap to a 733.33 MHz counter (Xbox TSC rate)

// Log the XBE return addresses found on a stack (heuristic: image values preceded by a call).
void log_stack_trace(const uint32_t *esp);
void log_stack_trace_ex(const uint32_t *esp, const uint32_t *top); // another thread's stack

// Suspend every Xbox thread and log where it is (used when a run times out).
void dump_threads();

// --probe: int3 at each address; logs registers and stack arguments, then single-steps and re-arms.
void install_probes();
void install_coverage();   // --coverage: one-shot int3 at each listed address, logged at the first hit
void log_coverage_summary(); // press-file "coverage": hit / not-hit lists
