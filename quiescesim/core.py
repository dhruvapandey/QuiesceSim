from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field
from enum import Enum
from hashlib import sha256
import copy
from typing import Callable, Mapping


State = dict[str, int]
Inputs = dict[str, int]
Transition = Callable[[Mapping[str, int], Mapping[str, int]], dict[str, int]]
QuiescenceProof = Callable[[Mapping[str, int], Mapping[str, int]], bool]


class Mode(str, Enum):
    OFF = "off"
    LEARN = "learn"
    GUIDED = "guided"
    GUARDED = "guarded"


@dataclass(frozen=True)
class Region:
    """A compiled clocked RTL region.

    `quiescent` is a user-supplied *proof obligation* for this small prototype:
    when it returns True, `transition` must return no writes.  Production code
    would derive and prove this condition from the elaborated RTL graph.
    """

    name: str
    reads: frozenset[str]
    writes: frozenset[str]
    transition: Transition
    quiescent: QuiescenceProof | None = None
    wake_inputs: frozenset[str] = frozenset()

    def can_skip(self, state: Mapping[str, int], inputs: Mapping[str, int]) -> bool:
        return self.quiescent is not None and self.quiescent(state, inputs)


@dataclass
class WaveChange:
    cycle: int
    signal: str
    old: int | None
    new: int


@dataclass
class Profile:
    fingerprint: str
    cycles: int = 0
    region_evaluations: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    region_state_changes: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    region_quiescent_cycles: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    guard_activations: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    wake_causes: dict[str, dict[str, int]] = field(
        default_factory=lambda: defaultdict(lambda: defaultdict(int))
    )

    def as_dict(self) -> dict:
        return {
            "fingerprint": self.fingerprint,
            "cycles": self.cycles,
            "region_evaluations": dict(self.region_evaluations),
            "region_state_changes": dict(self.region_state_changes),
            "region_quiescent_cycles": dict(self.region_quiescent_cycles),
            "guard_activations": dict(self.guard_activations),
            "wake_causes": {key: dict(value) for key, value in self.wake_causes.items()},
        }


@dataclass
class SimulationResult:
    state: State
    waves: list[WaveChange]
    checkpoints: dict[int, State]
    profile: Profile
    total_region_evaluations: int
    skipped_region_evaluations: int

    @property
    def avoided_percent(self) -> float:
        total = self.total_region_evaluations + self.skipped_region_evaluations
        return 0.0 if not total else 100.0 * self.skipped_region_evaluations / total


class Simulation:
    """Deterministic, cycle-based Phase-0 runtime.

    Each `tick` represents one rising edge of a common clock.  All regions see
    pre-edge state and their writes are committed together, matching the key
    nonblocking-assignment property needed by synchronous RTL.
    """

    def __init__(self, initial_state: State, regions: list[Region], *, observed: set[str] | None = None):
        self.initial_state = copy.deepcopy(initial_state)
        self.regions = list(regions)
        self.observed = observed or set(initial_state)
        write_owners: dict[str, str] = {}
        for region in self.regions:
            for signal in region.writes:
                if signal in write_owners:
                    raise ValueError(f"multiple writers for {signal}: {write_owners[signal]}, {region.name}")
                write_owners[signal] = region.name

    def fingerprint(self, test_name: str) -> str:
        material = repr((sorted(self.initial_state.items()), [(r.name, sorted(r.reads), sorted(r.writes)) for r in self.regions], test_name))
        return sha256(material.encode()).hexdigest()[:16]

    def run(
        self,
        stimuli: list[Inputs],
        *,
        test_name: str = "default",
        mode: Mode = Mode.OFF,
        checkpoint_every: int = 0,
    ) -> SimulationResult:
        state = copy.deepcopy(self.initial_state)
        previous_inputs: Inputs = {}
        waves: list[WaveChange] = []
        checkpoints: dict[int, State] = {0: copy.deepcopy(state)}
        profile = Profile(self.fingerprint(test_name))
        evaluations = skipped = 0

        for cycle, inputs in enumerate(stimuli, start=1):
            profile.cycles += 1
            old_state = copy.deepcopy(state)
            pending: dict[str, int] = {}
            changed_inputs = {key for key, value in inputs.items() if previous_inputs.get(key) != value}

            for region in self.regions:
                proven_quiescent = region.can_skip(old_state, inputs)
                if proven_quiescent:
                    profile.region_quiescent_cycles[region.name] += 1

                guarded_skip = mode == Mode.GUARDED and proven_quiescent
                wake_events = changed_inputs & region.wake_inputs
                if guarded_skip and wake_events:
                    guarded_skip = False
                    profile.guard_activations[region.name] += 1
                    for signal in wake_events:
                        profile.wake_causes[region.name][signal] += 1

                if guarded_skip:
                    skipped += 1
                    continue

                evaluations += 1
                profile.region_evaluations[region.name] += 1
                updates = region.transition(old_state, inputs)
                illegal = set(updates) - region.writes
                if illegal:
                    raise ValueError(f"{region.name} wrote undeclared signals: {sorted(illegal)}")
                if proven_quiescent and updates:
                    raise AssertionError(
                        f"unsound quiescence contract in {region.name}: proof allowed skipping but transition writes {updates}"
                    )
                if updates:
                    profile.region_state_changes[region.name] += 1
                pending.update(updates)

            for signal, new_value in pending.items():
                old_value = state.get(signal)
                if old_value != new_value:
                    state[signal] = new_value
                    if signal in self.observed:
                        waves.append(WaveChange(cycle, signal, old_value, new_value))

            if checkpoint_every and cycle % checkpoint_every == 0:
                checkpoints[cycle] = copy.deepcopy(state)
            previous_inputs = dict(inputs)

        return SimulationResult(state, waves, checkpoints, profile, evaluations, skipped)

    def replay_exact(self, stimuli: list[Inputs], checkpoint_cycle: int) -> SimulationResult:
        """Phase-0 replay API. Replays exactly from time zero to preserve semantics."""
        if checkpoint_cycle < 0 or checkpoint_cycle > len(stimuli):
            raise ValueError("checkpoint cycle is outside the supplied stimulus")
        return self.run(stimuli[:checkpoint_cycle], mode=Mode.OFF)
