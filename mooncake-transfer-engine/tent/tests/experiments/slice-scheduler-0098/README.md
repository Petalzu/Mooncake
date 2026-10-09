# TENT Slice Scheduler Preresearch (26fef0098)

This directory contains reproducible preparation for an online two-endpoint
Slice scheduler. It does not change the TENT runtime, and is not an RDMA
performance result or a merge-ready scheduler implementation.

The source baseline is `db6f14bd019d7fe08b2497e88046b72384af287f`.
The fork and official upstream had the same baseline when checked on 2026-10-10.
The YLT dependency is pinned to `7801bc9ad9021781f15217552214e325a1cf7373`.

## Contents

- `device-selector-probe.cpp` links the actual baseline DeviceSelector and
  Topology code with a virtual two-NIC topology. It checks actual-byte quota
  accounting, the extracted slice planner, and a fixed-rate two-path model.
- `path-model.cjs` extracts the slice planner verbatim and checks independent
  path models, stale-estimate counterexamples, shared-NIC resource bounds,
  and forced-exploration costs. It does not model actual RDMA queues.
- `mechanism-probe.cpp` is a standalone cache, affine-feedback and two-path
  integer-allocation prototype. Its data is synthetic and its time and byte
  units are normalized. It is not connected to Workers or SegmentManager.
- `run.sh` builds the unchanged baseline test sources and all three probes.
- `results/` contains a recorded Linux run, source hashes and raw GTest XML.

This directory fixes text files to LF line endings so the recorded source
hashes remain usable after Windows checkouts. Binary artifacts and dependency
archives are not committed; their hashes describe the recorded Linux run.

## Reproduce

The validated host used Ubuntu 22.04 amd64 and GCC 11.4.0. Required commands
are Git, GCC, Bash, Node.js, curl, tar, timeout, apt and dpkg-deb. The script
downloads fixed Ubuntu package versions and extracts them under its output
directory. It does not invoke sudo or install system packages. Package
availability and HTTPS access are prerequisites.

Run from a checkout containing this directory and the baseline Git object:

```bash
bash mooncake-transfer-engine/tent/tests/experiments/slice-scheduler-0098/run.sh
```

The default output is `build/slice-scheduler-0098`. Existing output directories
are rejected to preserve evidence. A new output path can be the first argument.
For an existing compatible dependency prefix, set `PRERESEARCH_DEPS_ROOT` to
the directory containing `include` and `lib/x86_64-linux-gnu`; the caller is
responsible for recording that prefix's dependency versions. This optional
path was used with the recorded Ubuntu package versions in the validation run.

The mechanism prototype alone has no external dependency:

```bash
g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -pedantic \
  mechanism-probe.cpp -o mechanism-probe
./mechanism-probe
```

## Verification Scope

The targeted baseline suite includes `device_selector_test`,
`strict_local_numa_test`, `bw_arbitration_test`, `quota_accounting_test`, and
`rail_monitor_test`. It uses C++20 and section garbage collection. The
prototype uses C++17; assertions remain enabled. The recorded suite has
92 completed cases with no failures or skips, including the earlier 67
cases rather than adding the two run totals together.

The mechanism report covers 11 cache invariants, 1,620 small two-path
allocation comparisons against exhaustive assignment, 12 fallback checks,
and 1,004 synthetic replay checks. The replay includes 1,000 requests over
five fixed seeds and four change/recovery/expiry checks. A bandwidth reversal
can regress the first request before feedback invalidates the old model.
Recovery observations at different sizes are explicitly supplied by the
simulation; a deployed exploration policy is not implemented here.

The cache supports owning snapshots, cross-thread publication, bounded
capacity and generation checks. Shared-pointer atomics are not assumed to
be lock-free. It does not yet implement a durable invalidation generation
or SegmentManager notification wiring. The integer planner is exhaustive
only for two independent paths and equal preceding slices plus one tail.
Shared-NIC cases remain resource-model counterexamples, not supported
adaptive topology cases. A residual margin is empirical, not a statistical
confidence bound or a guarantee against unobserved load changes.

ASan/UBSan are run on the standalone mechanism prototype only. The results
do not establish absence of data races; ThreadSanitizer was not run.
Full CMake/TENT integration, `rdma_transport_test`, actual RDMA throughput
or tail latency, and RISC-V64 validation have not been completed here.

## Contribution Status

This is a research branch for review and reproduction, not a request to
merge the experiments into upstream. AI assistance was used to prepare
code, checks and documentation. The eventual human PR submitter must review
the implementation and explain its limitations before proposing runtime
integration.
