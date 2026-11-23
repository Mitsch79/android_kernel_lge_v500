#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare a built V500 DTB with the normal scaling tables in LG's kernel."""
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("lg_repo", type=Path)
parser.add_argument("dtb", type=Path)
args = parser.parse_args()
revision = "386aad263c383fe56912413e98d190b483bc75f1"
base = "arch/arm/mach-msm/acpuclock-"

def source(file):
    return subprocess.check_output(
        ["git", "-C", args.lg_repo, "show", f"{revision}:{base}{file}"], text=True
    )

def fdt(*options):
    return subprocess.check_output(["fdtget", *options], text=True).strip()

def check(actual, expected, context):
    if actual != expected:
        raise ValueError(f"{context}: got {actual!r}, expected {expected!r}")

lg = source("8064.c")
enums = dict(re.findall(r"(PVS_\w+)\s*=\s*(\d+)", source("krait.h")))
plans = lg.split("static struct pvs_table pvs_tables[", 1)[1].split("};", 1)[0]
table = "/opp-table-cpu"
expected = {}
masks = {}
num_plans = 0
for speed, pvs, name, boost in re.findall(
    r"\[(\d+)\]\[(\w+)\]\s*=\s*\{\s*(\w+),\s*sizeof\(\w+\),\s*(\d+)", plans
):
    num_plans += 1
    pvs = enums.get(pvs, pvs)
    block = lg.split(f" {name}[] __initdata = {{", 1)[1].split("};", 1)[0]
    for khz, uv in re.findall(r"\{\s*1,\s*\{\s*(\d+),[^}]+\},\s*L2\(\d+\),\s*(\d+)", block):
        hz = int(khz) * 1000
        row = expected.setdefault(hz, {})
        masks[hz] = masks.get(hz, 0) | (1 << int(speed))
        for version, floor in ((0, 0), (1, 1150000)):
            prop = f"opp-microvolt-speed{speed}-pvs{pvs}-v{version}"
            row[prop] = max(int(uv), floor) + int(boost)

check(num_plans, 25, "LG plans")
check(set(fdt("-l", args.dtb, table).split()), {f"opp-{hz}" for hz in expected}, table)
for hz, row in expected.items():
    path = f"{table}/opp-{hz}"
    props = set(fdt("-p", args.dtb, path).split())
    check({p for p in props if p.startswith("opp-microvolt")}, set(row), path)
    check(fdt("-tu", args.dtb, path, "opp-hz"), f"0 {hz}", path)
    check(int(fdt("-tu", args.dtb, path, "opp-supported-hw")), masks[hz], path)
    status = fdt(args.dtb, path, "status") if "status" in props else "okay"
    check(status, "disabled" if hz > 1728000000 else "okay", path)
    for prop, uv in row.items():
        check(int(fdt("-tu", args.dtb, path, prop)), uv, f"{path}/{prop}")
print(f"{num_plans} plans, {len(expected)} frequency nodes, "
      f"{sum(map(len, expected.values()))} voltage properties verified")
