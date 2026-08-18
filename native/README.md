# QuiesceSim native semantic core

This C++ subsystem is the start of the native simulator, separate from the earlier Python feasibility demonstrator.

Current semantics:

- packed four-state words up to 64 bits;
- four-state bitwise X/Z propagation for the supported expression subset;
- deterministic active-region, nonblocking-assignment, and future-time queues;
- sensitivity wake-ups on value changes;
- value-change wave records;
- standard VCD export with initial values and timestamped changes;
- deterministic signal ordering for reproducible VCD/debug output;
- a guarded clock-enable fast path: an `always_ff` next-state expression is
  skipped only when its supported enable is definitely `0`; clocks and resets
  remain exact and every skip is counted.
- a regression test for reset, edge-triggered processes, NBA visibility, timed events, and X safety.

The initial restricted front end compiles literal-width `logic` declarations,
simple `assign` expressions, one single-assignment `always_comb` block, and either a simple posedge-only `always_ff`
or an asynchronous active-low-reset `always_ff` with an optional enable into native engine callbacks.
It rejects unsupported syntax rather than approximating it.
In particular, an unsupported procedural block is a build error even if other
parts of the module look supported; QuiesceSim must never silently simulate
only a subset of a DUT.

Current deliberate exclusions include parameters and parameterized widths,
unpacked arrays, `generate`/`for`, hierarchy, memories, multi-statement `always_comb`, complex
expressions, and general SystemVerilog testbenches. Real Ibex RTL is expected
to fail at this boundary today. The next front-end milestone is an elaborated,
standards-aware representation; it is not to grow this regex parser until it
accidentally accepts Ibex.

This is not yet a SystemVerilog parser or an Ibex-capable simulator. Its role is to make the exact runtime correct and testable before a front end and acceleration are added.

The current tree also contains one explicitly hand-lowered, known-input
reference kernel for Ibex's resolved 64-bit counter. It is a native-runtime
validation step, not evidence that the compiler can yet execute Ibex end to end.
`examples/ibex_counter64_tb.sv` applies the same test to the original Ibex
source for differential checking.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/quiescesim_run examples/counter.sv
./build/quiescesim_run examples/counter.sv --vcd counter.vcd
./build/quiescesim_ibex_counter64_run
```

`examples/counter_tb.sv` is a matching reference testbench for differential
checks with an established simulator. It validates the small supported slice;
it is not an Ibex test or a general compatibility claim.
