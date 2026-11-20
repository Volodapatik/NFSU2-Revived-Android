#!/usr/bin/env python3
"""Apply beta24-only passive guest-state telemetry to lifted NFSU2 C sources."""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

FUNCTION_START = re.compile(r"^\s*void\s+sub_([0-9A-Fa-f]{8})\s*\(")
DIRECT_STORE = re.compile(
    r"^(?P<indent>[ \t]*)MEM32\(0x0*870CD4\)\s*=\s*(?P<value>.*?);\s*/\*\s*0x(?P<pc>[0-9A-Fa-f]{8}):",
    re.IGNORECASE,
)
GUEST_PC = re.compile(r"/\*\s*0x([0-9A-Fa-f]{8}):")
SOURCE_NAME = re.compile(r"recomp_[0-9]{4}\.c$")
SNAPSHOT_MARKERS = {
    "ba_entry": "NFS_GUEST_STATE_TRACE_BA_ENTRY",
    "ba_exit": "NFS_GUEST_STATE_TRACE_BA_EXIT",
    "caa_entry": "NFS_GUEST_STATE_TRACE_CAA_ENTRY",
}


def snapshot_call(indent: str, kind: str, pc: int, function: int) -> str:
    return (
        f"{indent}nfsD3D9RecordGuestStateTrace({kind}, 0x{pc:08X}u, "
        "MEM32(0x870CD4), MEM32(0x870CD4), MEM32(0x87099C), "
        f"0x{function:08X}u, esp, ebp, MEM32(esp));\n"
    )


def instrument_file(path: Path) -> dict[str, int]:
    """Stream one generated C file through an atomic rewrite; safe to re-run."""
    temp = path.with_name(path.name + ".beta24.tmp")
    with path.open("r", encoding="utf-8", errors="strict", newline="") as source:
        has_probe_include = False
        existing = {key: False for key in SNAPSHOT_MARKERS}
        for line in source:
            if '"guest_render_probe.h"' in line:
                has_probe_include = True
            for key, marker in SNAPSHOT_MARKERS.items():
                if marker in line:
                    existing[key] = True
        source.seek(0)
        counts = {"direct_stores": 0, "ba_entry": int(existing["ba_entry"]),
                  "ba_exit": int(existing["ba_exit"]), "caa_entry": int(existing["caa_entry"])}
        current_func: int | None = None
        previous_output = ""
        header_inserted = has_probe_include
        try:
            with temp.open("w", encoding="utf-8", newline="") as output:
                for line in source:
                    function = FUNCTION_START.match(line)
                    if function:
                        current_func = int(function.group(1), 16)

                    if not header_inserted and line.strip() == '#include "recomp_funcs.h"':
                        output.write(line)
                        output.write('#include "guest_render_probe.h"\n')
                        header_inserted = True
                        previous_output = '#include "guest_render_probe.h"\n'
                        continue

                    direct = DIRECT_STORE.match(line)
                    if direct:
                        counts["direct_stores"] += 1
                        if current_func is None:
                            raise RuntimeError(f"target write outside a lifted function: {path}")
                        if "NFS_GUEST_STATE_TRACE_DIRECT_WRITE" not in previous_output:
                            indent = direct.group("indent")
                            value = direct.group("value").strip()
                            pc = int(direct.group("pc"), 16)
                            output.write(
                                f"{indent}nfsD3D9RecordGuestStateTrace(NFS_GUEST_STATE_TRACE_DIRECT_WRITE, "
                                f"0x{pc:08X}u, MEM32(0x870CD4), (uint32_t)({value}), "
                                f"MEM32(0x87099C), 0x{current_func:08X}u, esp, ebp, MEM32(esp));\n"
                            )
                        output.write(line)
                        previous_output = line
                        continue

                    if current_func == 0x005BA020 and "RECOMP_ENTER(0x005BA020u);" in line:
                        output.write(line)
                        if not existing["ba_entry"]:
                            indent = line[: len(line) - len(line.lstrip())]
                            output.write(snapshot_call(indent, SNAPSHOT_MARKERS["ba_entry"], 0x005BA020, current_func))
                            counts["ba_entry"] += 1
                            existing["ba_entry"] = True
                            previous_output = SNAPSHOT_MARKERS["ba_entry"]
                        else:
                            previous_output = line
                        continue

                    if current_func == 0x005CAA20 and "RECOMP_ENTER(0x005CAA20u);" in line:
                        output.write(line)
                        if not existing["caa_entry"]:
                            indent = line[: len(line) - len(line.lstrip())]
                            output.write(snapshot_call(indent, SNAPSHOT_MARKERS["caa_entry"], 0x005CAA20, current_func))
                            counts["caa_entry"] += 1
                            existing["caa_entry"] = True
                            previous_output = SNAPSHOT_MARKERS["caa_entry"]
                        else:
                            previous_output = line
                        continue

                    if current_func == 0x005BA020 and "RECOMP_RETURN()" in line:
                        if not existing["ba_exit"]:
                            match = GUEST_PC.search(line)
                            pc = int(match.group(1), 16) if match else 0x005BA020
                            indent = line[: len(line) - len(line.lstrip())]
                            output.write(snapshot_call(indent, SNAPSHOT_MARKERS["ba_exit"], pc, current_func))
                            counts["ba_exit"] += 1
                            existing["ba_exit"] = True
                        output.write(line)
                        previous_output = line
                        continue

                    output.write(line)
                    previous_output = line
            os.replace(temp, path)
        except Exception:
            try:
                temp.unlink()
            except FileNotFoundError:
                pass
            raise
    return counts


def instrument_tree(root: str | os.PathLike[str]) -> dict[str, int]:
    directory = Path(root)
    sources = sorted(path for path in directory.glob("recomp_*.c") if SOURCE_NAME.fullmatch(path.name))
    if not sources:
        raise RuntimeError(f"no generated recomp source files found under {directory}")
    totals = {"files": 0, "direct_stores": 0, "ba_entry": 0, "ba_exit": 0, "caa_entry": 0}
    for source in sources:
        result = instrument_file(source)
        totals["files"] += 1
        for key, value in result.items():
            totals[key] += value
    if not totals["direct_stores"] or not totals["ba_entry"] or not totals["ba_exit"] or not totals["caa_entry"]:
        raise RuntimeError(f"beta24 trace anchors missing from generated sources: {totals}")
    return totals


if __name__ == "__main__":
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "src/recomp/gen"
    print(instrument_tree(root))
