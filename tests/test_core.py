import unittest

from quiescesim.core import Mode
from quiescesim.example import build_demo, demo_stimuli


class QuiesceSimTests(unittest.TestCase):
    def test_guarded_and_exact_have_identical_final_state_and_waves(self):
        sim = build_demo()
        stimuli = demo_stimuli(100)
        exact = sim.run(stimuli, mode=Mode.OFF, checkpoint_every=25)
        guarded = sim.run(stimuli, mode=Mode.GUARDED, checkpoint_every=25)

        self.assertEqual(guarded.state, exact.state)
        self.assertEqual(
            [(w.cycle, w.signal, w.new) for w in guarded.waves],
            [(w.cycle, w.signal, w.new) for w in exact.waves],
        )
        self.assertGreater(guarded.skipped_region_evaluations, 0)
        self.assertLess(guarded.total_region_evaluations, exact.total_region_evaluations)


    def test_boundary_write_reactivates_a_quiescent_region(self):
        sim = build_demo()
        result = sim.run(demo_stimuli(100), mode=Mode.GUARDED)
        self.assertEqual(result.state["peripheral_reg"], 0xA5)
        # Both assertion and deassertion of a guarded boundary are observed.
        self.assertEqual(result.profile.guard_activations["soc.unused_peripheral"], 2)
        self.assertEqual(result.profile.wake_causes["soc.unused_peripheral"]["peripheral_write"], 2)


    def test_bad_quiescence_proof_is_rejected(self):
        sim = build_demo()
        broken = sim.regions[0]
        sim.regions[0] = type(broken)(
            name=broken.name,
            reads=broken.reads,
            writes=broken.writes,
            transition=broken.transition,
            quiescent=lambda state, inputs: True,
            wake_inputs=broken.wake_inputs,
        )
        with self.assertRaisesRegex(AssertionError, "unsound quiescence"):
            sim.run(demo_stimuli(10), mode=Mode.OFF)
