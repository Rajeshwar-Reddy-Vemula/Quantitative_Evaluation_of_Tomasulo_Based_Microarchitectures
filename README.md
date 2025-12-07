# Quantitative_Evaluation_of_Tomasulo_Based_Microarchitectures
A C++ simulator that models three versions of Tomasulo's Algorithm. Model A does Dynamic Scheduling, Model B has dynamic scheduling, RoB but no Speculation and Model C has both along with  a single bit naive branch predictor. So both Model A and B stall on branches but Model B provides precise exceptions.

Tomasulo Simulator
===========================

1. Build
Make sure you have a C++ compiler (e.g., g++) that supports at least C++11.

Build command:
    g++ -std=c++11 tomasulo_abc.cpp -o tomasulo

This produces an executable named "tomasulo".

2. Trace File
Prepare a trace file (e.g., source.txt) that contains:
- Configuration directives:
  - ADD_SUB_RESERVATION_STATIONS
  - MUL_DIV_RESERVATION_STATIONS
  - LOAD_BUFFERS, STORE_BUFFERS
  - ADD_SUB_CYCLES, MUL_CYCLES, DIV_CYCLES, LOAD_STORE_CYCLES
  - REG
  - INIT_REG Fk value
  - INIT_MEM addr value
- Instruction list:
  - ADD, SUB, MUL, DIV
  - LOAD, STORE
  - BEQ, BNEQ

The project’s example trace can be used directly or modified.

3. Running the Simulator
The simulator has three models:
- Model A: classic Tomasulo (no RoB, no speculation)
- Model B: Tomasulo + 32-entry RoB, no speculation
- Model C: Tomasulo + 32-entry RoB + simple branch prediction (speculation)

Usage:
- Model A with default trace (source.txt):
    ./tomasulo A

- Model B with an explicit trace:
    ./tomasulo B mytrace.txt

- Model C with an explicit trace:
    ./tomasulo C mytrace.txt

If no trace file is given, the simulator uses "source.txt" in the current directory.

4. Interactive Stepping
The simulator runs cycle-by-cycle. At each cycle it prints:
- Register file contents
- Instruction status table (issue, execute, commit cycles)
- RAT / reservation stations / load-store buffers
- RoB contents (for Models B and C)

After printing, it prompts:
    Press Enter to advance, or 'q' + Enter to quit:

Controls:
- Press Enter to advance one cycle.
- Press q followed by Enter to stop the simulation early.

At the end of the run, the simulator prints:
- Total cycles
- Number of committed instructions
- CPI
- Final register state
- Final memory (touched locations)
