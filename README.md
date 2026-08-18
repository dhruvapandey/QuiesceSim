# QuiesceSim — Phase 0 demonstrator

This is a runnable proof-of-concept for the core QuiesceSim idea: **guarded semantic clock skipping**. It is not a Verilog/SystemVerilog parser or a VCS replacement.

It demonstrates a deterministic cycle runtime in which regions are represented by compiled transition functions. A region may skip a clock-edge evaluation only when it supplies a quiescence contract. Boundary input changes trigger an exact evaluation rather than allowing a stale summary.

## Run

```bash
cd outputs/quiescesim
PYTHONPATH=. python3 -m unittest discover -s tests -v
PYTHONPATH=. python3 -m quiescesim.cli compare --cycles 100
PYTHONPATH=. python3 -m quiescesim.cli run --mode guarded --cycles 100
```

The demo has an active DMA and an otherwise idle peripheral. The guarded run produces the same final state and observed wave transitions as exact mode, while avoiding evaluations of proven-quiescent regions. A peripheral bus write deliberately triggers reactivation.

## Safety invariant

AI or training profiles may propose candidates, but they cannot authorize skipping. The runtime skips only when:

1. the quiescence contract holds;
2. no declared wake boundary changed; and
3. the transition function would make no writes under that contract.

The third condition is checked by the prototype when the region is evaluated. A production implementation must replace user-supplied contracts with a proof over elaborated RTL and retain exact event-driven fallback semantics.

## Included Phase-0 components

- deterministic nonblocking-style commit at a common clock edge;
- exact, learn, guided, and guarded modes;
- reusable fingerprinted activity profile;
- region state-change, quiescence, wake-cause, and avoided-work reporting;
- selective wave-change records;
- checkpoints and exact replay API;
- differential tests comparing exact and guarded execution.

## Deliberately not included

SystemVerilog parsing/elaboration, general event queues, delta cycles, multiple clocks, four-state resolution, UVM, DPI/VPI, coverage, production trace formats, and automatic formal proof generation. Those are subsequent workstreams required for a VCS-class product.
