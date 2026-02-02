# Out-of-Order Superscalar Processor Simulator

## Overview
This project from UCLA CS 116C implements a **cycle-accurate C++ simulator for an out-of-order superscalar processor**, developed as part of a computer architecture course. The simulator models instruction-level parallelism and realistic microarchitectural behavior while executing traces with over **10,000 dynamic instructions**.

The focus of the project is correctness, timing accuracy, and performance analysis under different architectural configurations.

---

## Processor Model
The simulator models a **5-stage pipeline**:

1. Fetch  
2. Dispatch  
3. Schedule (Reservation Stations)  
4. Execute (Functional Units)  
5. State Update  

Key features include:
- Out-of-order execution  
- Reservation stations  
- Multiple functional units  
- Common Data Buses (CDBs)  
- Cycle-accurate latch and half-cycle timing  
- Tag-ordered issue and completion  

---

## Key Parameters
- **N (Issue Width)**  
  Number of instructions that can be fetched and dispatched per cycle.

- **R (Result Bus Count)**  
  Number of Common Data Buses available to broadcast completed instruction results per cycle.

These parameters are configurable and were varied to analyze performance bottlenecks.

---

## Implementation Details
- Written in **C++**
- Simulates **cycle-by-cycle pipeline behavior**
- Enforces **half-cycle semantics** for result broadcast and functional unit freeing
- Tracks per-stage queues, dependencies, and resource availability
- Produces detailed per-cycle logs for validation

---

## Performance Debugging
Initial testing showed lower-than-expected IPC despite available execution resources. To diagnose this:

- Profiled simulator runtime using **Linux `perf`**
- Correlated profiling data with:
  - Cycle-by-cycle pipeline logs
  - Per-stage counters
  - IPC and total cycle statistics

This revealed **result bus (CDB) contention** as the primary bottleneck, where completed instructions stalled waiting for limited completion bandwidth.

---

## Optimizations
- Enforced **strict tag-ordered result bus arbitration**
- Freed functional units at the correct **half-cycle boundary**
- Optimized reservation-station scanning by tracking ready-to-complete instructions

---

## Results
- Completed **16 fully correct traces**
- Validated across multiple configurations:
  - Baseline  
  - **N = 8** (wider issue width)  
  - **R = 4** (additional result buses)  
  - **N = 8, R = 4** (combined)  
- Improved IPC and reduced total execution cycles on high-ILP workloads
- Achieved **exact matches to golden per-cycle logs (diff = 0)** across all runs

---

## Build & Run
```bash
make
./procsim -r R -f F -j k0 -k k1 -l k2 < trace_file
