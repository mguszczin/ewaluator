# Concurrent Policy Evaluator

## Overview

The **Concurrent Policy Evaluator** is a highly concurrent, POSIX-compliant C++ application designed to evaluate Machine Learning policies (e.g., Deep Neural Networks) within simulated environments.

It executes tests asynchronously by managing interactions between a **Policy** (which takes a state and returns an action) and an **Environment** (which takes an action and returns a new state), repeating this cycle until a terminal state is reached.

Due to strict system-level constraints (explicit prohibition of threading libraries like `pthread` and I/O multiplexing like `poll`/`select`), this application implements a custom multi-process, signal-driven architecture using `fork`, `pipe`, and `SIGUSR1` interrupts to achieve non-blocking concurrency.

---

## Architecture

To bypass the restriction on multiplexing system calls, the evaluator relies on a master-worker architecture:

- **Main Evaluator**  
  Maintains state machines for all active tests and manages resource scheduling.

- **Reader Process**  
  A dedicated subprocess that continuously reads standard input for new test cases and pushes them to the main evaluator via an event pipe.

- **Waiter Processes**  
  Small proxy subprocesses attached to the output pipes of Policies and Environments. They perform blocking reads and, upon receiving data, send a `SIGUSR1` signal to the main process and pass the data through a centralized event pipe.

- **Environment & Policy Processes**  
  External executables invoked via `execv`. They communicate with the system exclusively through standard input and output.

---

## Resource Constraints

The application strictly enforces the following resource limits to simulate real-world hardware constraints (e.g., GPU, CPU, and Memory limits):

- **maxConcurrentPolicyCalls (GPU Limit)**  
  The maximum number of policies calculating an action simultaneously.

- **maxConcurrentCalls (CPU Limit)**  
  The combined maximum number of policies and environments calculating simultaneously.

- **maxActiveEnvironments (Memory Limit)**  
  The maximum number of instantiated environment processes residing in memory.

---

## Usage

```bash
./evaluator <policyPath> <envPath> <maxConcurrentPolicyCalls> <maxConcurrentCalls> <maxActiveEnvironments> [extraArgs...]
```

- `policyPath` – Path to the policy executable  
- `envPath` – Path to the environment executable  
- `maxConcurrentPolicyCalls` – Integer limit for concurrent policies  
- `maxConcurrentCalls` – Integer limit for total concurrent calculations (policies + environments)  
- `maxActiveEnvironments` – Integer limit for active environment processes  
- `extraArgs` – Optional arguments passed directly to the policy and environment subprocesses  

#### Example
```bash
echo -ne "TEST0001\nTEST0002\n" | setsid ./evaluator ./demo_policy ./demo_env 2 4 10
```
---

## Signals & Exit Codes

- `0` – Success (all tests processed)  
- `1` – General error or invalid arguments  
- `2` – SIGINT (Ctrl+C) received. Triggers immediate cleanup of all child processes, pipes, and file descriptors before exiting  
- `EOF` – Closes input stream, finishes currently queued tests, cleans up resources, and exits with `0`  

## Example of Building the Project

This project uses **CMake**. A specific compile definition (`ORDERED_OUTPUT=1`) is required to ensure the results are printed in the exact order they were received on standard input.

```bash
mkdir build && cd build
cmake -S ../ -B . -DCMAKE_BUILD_TYPE=Release -DORDERED_OUTPUT=1
make evaluator
```
