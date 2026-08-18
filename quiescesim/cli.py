from __future__ import annotations

import argparse
import json
from pathlib import Path

from .core import Mode
from .elaborate import elaborate_with_verilator, fingerprint_file, lower_resolved_module
from .example import build_demo, demo_stimuli


def main() -> None:
    parser = argparse.ArgumentParser(description="QuiesceSim Phase-0 demonstrator")
    parser.add_argument("command", choices=["run", "compare", "elaborate"])
    parser.add_argument("--mode", choices=[m.value for m in Mode], default=Mode.GUARDED.value)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--checkpoint-every", type=int, default=25)
    parser.add_argument("--file-list", type=Path, help="Verilator file list used by the bootstrap front end")
    parser.add_argument("--output-dir", type=Path, help="Directory for imported elaboration artifacts")
    parser.add_argument("--manifest", type=Path, help="Write a compact QuiesceSim-owned elaboration manifest")
    parser.add_argument("--lower-module", help="Resolved module name to export for native lowering")
    parser.add_argument("--lowered-ir", type=Path, help="Destination for the resolved-module lowering artifact")
    args = parser.parse_args()

    if args.command == "elaborate":
        if args.file_list is None or args.output_dir is None:
            parser.error("elaborate requires --file-list and --output-dir")
        design = elaborate_with_verilator(args.file_list, args.output_dir)
        json_file = next(args.output_dir.glob("*.tree.json"))
        if args.manifest is not None:
            design.write_manifest(args.manifest, source_json=json_file, source_fingerprint=fingerprint_file(args.file_list))
        if args.lower_module is not None or args.lowered_ir is not None:
            if args.lower_module is None or args.lowered_ir is None:
                parser.error("--lower-module and --lowered-ir must be used together")
            lower_resolved_module(json_file, args.lower_module).write(args.lowered_ir)
        print(json.dumps({"frontend": "verilator-bootstrap", **design.as_dict()}, indent=2))
        return

    sim = build_demo()
    stimuli = demo_stimuli(args.cycles)
    if args.command == "compare":
        exact = sim.run(stimuli, mode=Mode.OFF, checkpoint_every=args.checkpoint_every)
        guarded = sim.run(stimuli, mode=Mode.GUARDED, checkpoint_every=args.checkpoint_every)
        print(json.dumps({
            "equivalent_final_state": exact.state == guarded.state,
            "exact_evaluations": exact.total_region_evaluations,
            "guarded_evaluations": guarded.total_region_evaluations,
            "avoided_percent": round(guarded.avoided_percent, 2),
            "guard_activations": dict(guarded.profile.guard_activations),
        }, indent=2))
        return

    result = sim.run(stimuli, mode=Mode(args.mode), checkpoint_every=args.checkpoint_every)
    print(json.dumps({
        "mode": args.mode,
        "final_state": result.state,
        "region_evaluations": result.total_region_evaluations,
        "skipped_region_evaluations": result.skipped_region_evaluations,
        "avoided_percent": round(result.avoided_percent, 2),
        "profile": result.profile.as_dict(),
        "wave_changes": [change.__dict__ for change in result.waves],
        "checkpoints": sorted(result.checkpoints),
    }, indent=2))


if __name__ == "__main__":
    main()
