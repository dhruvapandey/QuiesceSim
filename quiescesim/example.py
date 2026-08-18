"""A tiny SoC-style model used to demonstrate semantic clock skipping."""

from .core import Region, Simulation


def build_demo() -> Simulation:
    # active DMA path advances while `dma_start` is set; idle peripheral has a
    # live clock but holds its state unless a bus write or reset arrives.
    dma = Region(
        name="soc.dma",
        reads=frozenset({"dma_busy", "dma_addr", "dma_remaining", "reset_n"}),
        writes=frozenset({"dma_busy", "dma_addr", "dma_remaining"}),
        transition=lambda s, i: (
            {"dma_busy": 0, "dma_remaining": 0}
            if not i.get("reset_n", 1)
            else ({"dma_busy": 1, "dma_addr": s["dma_addr"] + 4, "dma_remaining": s["dma_remaining"] - 1}
                  if s["dma_busy"] and s["dma_remaining"] > 1
                  else ({"dma_busy": 0, "dma_remaining": 0}
                        if s["dma_busy"] else ({"dma_busy": 1, "dma_remaining": i["dma_len"]} if i.get("dma_start", 0) else {})))
        ),
        quiescent=lambda s, i: not i.get("reset_n", 1) or (not s["dma_busy"] and not i.get("dma_start", 0)),
        wake_inputs=frozenset({"reset_n", "dma_start"}),
    )
    peripheral = Region(
        name="soc.unused_peripheral",
        reads=frozenset({"peripheral_reg", "reset_n", "peripheral_write", "peripheral_wdata"}),
        writes=frozenset({"peripheral_reg"}),
        transition=lambda s, i: (
            {"peripheral_reg": 0}
            if not i.get("reset_n", 1)
            else ({"peripheral_reg": i["peripheral_wdata"]} if i.get("peripheral_write", 0) else {})
        ),
        quiescent=lambda s, i: i.get("reset_n", 1) and not i.get("peripheral_write", 0),
        wake_inputs=frozenset({"reset_n", "peripheral_write", "peripheral_wdata"}),
    )
    return Simulation(
        {"dma_busy": 0, "dma_addr": 0, "dma_remaining": 0, "peripheral_reg": 0},
        [dma, peripheral],
        observed={"dma_busy", "dma_addr", "dma_remaining", "peripheral_reg"},
    )


def demo_stimuli(cycles: int = 100) -> list[dict[str, int]]:
    stimuli = []
    for cycle in range(cycles):
        stimuli.append(
            {
                "reset_n": 1,
                "dma_start": int(cycle == 5),
                "dma_len": 8,
                "peripheral_write": int(cycle == 70),
                "peripheral_wdata": 0xA5,
            }
        )
    return stimuli
