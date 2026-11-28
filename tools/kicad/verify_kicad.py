#!/usr/bin/env python3
"""
KiCad 8/9 S-expression validator, geometry/connectivity ERC checker, and SVG preview renderer.

Parses .kicad_pro, .kicad_sch, quad_ipkvm.kicad_sym, and sym-lib-table directly from disk:
1. Valid JSON .kicad_pro + balanced S-expressions (StrAtom-safe) + KiCad 8 version 20231120.
2. Globally unique UUIDs across all schematic files; graceful diagnostics on malformed root.
3. Project symbol library integrity: verifies sym-lib-table & .kicad_sym exist and contain every
   quad_ipkvm:* symbol; verifies embedded lib_symbols and no Lib: prefix on sub-symbols.
4. Project-wide unique non-power Ref+Unit pairs and valid (instances ...) hierarchy paths.
5. Strict 1.27 mm grid checks on wires, pins, junctions, no_connects, labels, hierarchical_labels.
6. Zero floating pins, zero dangling wire ends, zero interior pin/wire-end T violations,
   explicit junctions on 3+-way intersections.
7. Non-power symbol bbox overlap checks across rects, circles, and polylines of the active unit.
8. Power rail ERC check: traces net graph across wires + power net names and ensures each power net
   (+3V3, GND) has a power_out driver without conflicting duplicate power_out pins.
9. Renders dynamic A2/A3 SVG previews (with vertical & horizontal pin numbers + dashed zone frames).
"""

from __future__ import annotations

import argparse
import html
import json
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

GRID = 1.27
EPS = 0.02

PAPER_SIZES = {
    "A2": (594.0, 420.0),
    "A3": (420.0, 297.0),
}


class StrAtom(str):
    """Distinguishes quoted string literals from bare symbols/parens."""


def parse_sexpr(text: str) -> List[Any]:
    tokens: List[Any] = []
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c in ("(", ")"):
            tokens.append(c)
            i += 1
        elif c == '"':
            i += 1
            buf: List[str] = []
            while i < n:
                ch = text[i]
                if ch == "\\":
                    i += 1
                    if i < n:
                        esc = text[i]
                        buf.append("\n" if esc == "n" else esc)
                        i += 1
                elif ch == '"':
                    i += 1
                    break
                else:
                    buf.append(ch)
                    i += 1
            else:
                raise ValueError("Unterminated quoted string in S-expression")
            tokens.append(StrAtom("".join(buf)))
        else:
            start = i
            while i < n and not text[i].isspace() and text[i] not in ("(", ")", '"'):
                i += 1
            tokens.append(text[start:i])

    stack: List[List[Any]] = [[]]
    for tok in tokens:
        if not isinstance(tok, StrAtom) and tok == "(":
            lst: List[Any] = []
            stack[-1].append(lst)
            stack.append(lst)
        elif not isinstance(tok, StrAtom) and tok == ")":
            if len(stack) == 1:
                raise ValueError("Unexpected ')' in S-expression")
            stack.pop()
        else:
            stack[-1].append(tok)
    if len(stack) != 1:
        raise ValueError("Unclosed '(' in S-expression")
    return stack[0]


def find_children(node: List[Any], tag: str) -> List[List[Any]]:
    return [c for c in node if isinstance(c, list) and len(c) > 0 and c[0] == tag]


def find_child(node: List[Any], tag: str) -> Optional[List[Any]]:
    for c in node:
        if isinstance(c, list) and len(c) > 0 and c[0] == tag:
            return c
    return None


def rotate_ccw(px: float, py: float, angle: int) -> Tuple[float, float]:
    a = angle % 360
    if a == 0:
        return (px, py)
    if a == 90:
        return (-py, px)
    if a == 180:
        return (-px, -py)
    if a == 270:
        return (py, -px)
    raise ValueError(f"Unsupported symbol angle {angle}")


def pin_to_canvas(sx: float, sy: float, angle: int, px: float, py: float) -> Tuple[float, float]:
    rx, ry = rotate_ccw(px, py, angle)
    return (round(sx + rx, 2), round(sy - ry, 2))


def sym_pt_to_canvas(sx: float, sy: float, angle: int, lx: float, ly: float) -> Tuple[float, float]:
    rx, ry = rotate_ccw(lx, ly, angle)
    return (round(sx + rx, 2), round(sy - ry, 2))


def is_on_grid(val: float) -> bool:
    rem = abs(val / GRID - round(val / GRID)) * GRID
    return rem <= EPS


def pt_on_seg_interior(p: Tuple[float, float], a: Tuple[float, float], b: Tuple[float, float]) -> bool:
    """Return True if p lies strictly on segment ab (not at endpoints)."""
    px, py = p
    ax, ay = a
    bx, by = b
    if (abs(px - ax) < EPS and abs(py - ay) < EPS) or (abs(px - bx) < EPS and abs(py - by) < EPS):
        return False
    dx, dy = bx - ax, by - ay
    seg_len2 = dx * dx + dy * dy
    if seg_len2 < EPS * EPS:
        return False
    cross = abs((px - ax) * dy - (py - ay) * dx)
    if cross / (seg_len2 ** 0.5) > EPS:
        return False
    dot = (px - ax) * dx + (py - ay) * dy
    return 0.0 < dot < seg_len2


def qpt(pt: Tuple[float, float]) -> Tuple[int, int]:
    return (int(round(pt[0] / GRID)), int(round(pt[1] / GRID)))


@dataclass
class ParsedPin:
    num: str
    name: str
    etype: str
    x: float
    y: float
    angle: int
    length: float
    unit: int
    hide: bool


@dataclass
class ParsedLibSym:
    lib_id: str
    is_power: bool
    pins: List[ParsedPin]
    rects: List[Tuple[int, float, float, float, float]]
    polys: List[Tuple[int, List[Tuple[float, float]]]]
    circles: List[Tuple[int, float, float, float]]


def parse_lib_symbol(node: List[Any], require_prefix: bool = True) -> Tuple[ParsedLibSym, List[str]]:
    errs: List[str] = []
    lib_id = str(node[1])
    if require_prefix and ":" not in lib_id:
        errs.append(f"lib_symbol {lib_id!r} missing Lib:Name format")
    is_power = find_child(node, "power") is not None
    pins: List[ParsedPin] = []
    rects: List[Tuple[int, float, float, float, float]] = []
    polys: List[Tuple[int, List[Tuple[float, float]]]] = []
    circles: List[Tuple[int, float, float, float]] = []

    for sub in find_children(node, "symbol"):
        sub_name = str(sub[1])
        if ":" in sub_name:
            errs.append(f"Sub-symbol {sub_name!r} in {lib_id} must NOT contain 'Lib:' prefix")
        parts = sub_name.rsplit("_", 2)
        unit = int(parts[1]) if len(parts) == 3 and parts[1].isdigit() else 0

        for r in find_children(sub, "rectangle"):
            st = find_child(r, "start")
            en = find_child(r, "end")
            if st and en:
                rects.append((unit, float(st[1]), float(st[2]), float(en[1]), float(en[2])))

        for pl in find_children(sub, "polyline"):
            pts_n = find_child(pl, "pts")
            if pts_n:
                pts = [(float(xy[1]), float(xy[2])) for xy in find_children(pts_n, "xy")]
                polys.append((unit, pts))

        for ci in find_children(sub, "circle"):
            ce = find_child(ci, "center")
            ra = find_child(ci, "radius")
            if ce and ra:
                circles.append((unit, float(ce[1]), float(ce[2]), float(ra[1])))

        for pn in find_children(sub, "pin"):
            etype = str(pn[1])
            at_n = find_child(pn, "at")
            len_n = find_child(pn, "length")
            nm_n = find_child(pn, "name")
            no_n = find_child(pn, "number")
            hide = "hide" in pn
            pins.append(ParsedPin(
                num=str(no_n[1]) if no_n else "?",
                name=str(nm_n[1]) if nm_n else "",
                etype=etype,
                x=float(at_n[1]) if at_n else 0.0,
                y=float(at_n[2]) if at_n else 0.0,
                angle=int(float(at_n[3])) if at_n and len(at_n) > 3 else 0,
                length=float(len_n[1]) if len_n else 0.0,
                unit=unit,
                hide=hide,
            ))
    return ParsedLibSym(lib_id, is_power, pins, rects, polys, circles), errs


def collect_uuids(node: Any, seen: Dict[str, str], file_label: str, errs: List[str]) -> None:
    if isinstance(node, list):
        if len(node) >= 2 and node[0] == "uuid":
            u = str(node[1])
            if u in seen:
                errs.append(f"Duplicate UUID {u} in {file_label} (already in {seen[u]})")
            else:
                seen[u] = file_label
        for ch in node:
            collect_uuids(ch, seen, file_label, errs)


def validate_sheet(
    sch_path: Path,
    expected_hier_path: str,
    is_root: bool,
    seen_uuids: Dict[str, str],
    seen_ref_units: Set[Tuple[str, int]],
    project_lib_syms: Optional[Set[str]],
) -> Tuple[List[str], Optional[Dict[str, Any]]]:
    errs: List[str] = []
    try:
        ast_roots = parse_sexpr(sch_path.read_text(encoding="utf-8"))
    except ValueError as exc:
        return [f"{sch_path.name}: S-expression parse error: {exc}"], None

    if len(ast_roots) != 1 or not isinstance(ast_roots[0], list) or not ast_roots[0] or ast_roots[0][0] != "kicad_sch":
        return [f"{sch_path.name}: top-level node must be (kicad_sch ...)"], None

    sch = ast_roots[0]
    ver_n = find_child(sch, "version")
    if not ver_n or int(ver_n[1]) != 20231120:
        errs.append(f"{sch_path.name}: expected (version 20231120), got {ver_n}")

    uuid_n = find_child(sch, "uuid")
    sheet_uuid = str(uuid_n[1]) if uuid_n and len(uuid_n) > 1 else ""
    if not sheet_uuid:
        errs.append(f"{sch_path.name}: missing top-level uuid")

    paper_n = find_child(sch, "paper")
    paper_name = str(paper_n[1]) if paper_n and len(paper_n) > 1 else "A3"
    if paper_name not in PAPER_SIZES:
        errs.append(f"{sch_path.name}: unsupported paper {paper_name!r}")

    collect_uuids(sch, seen_uuids, sch_path.name, errs)

    lib_syms: Dict[str, ParsedLibSym] = {}
    lib_root = find_child(sch, "lib_symbols")
    if lib_root:
        for sym_n in find_children(lib_root, "symbol"):
            ls, lerrs = parse_lib_symbol(sym_n, require_prefix=True)
            errs.extend([f"{sch_path.name}: {e}" for e in lerrs])
            lib_syms[ls.lib_id] = ls

    if is_root and find_child(sch, "sheet_instances") is None:
        errs.append(f"{sch_path.name}: root sheet missing (sheet_instances ...)")

    wires: List[Tuple[Tuple[float, float], Tuple[float, float]]] = []
    for w in find_children(sch, "wire"):
        pts_n = find_child(w, "pts")
        xys = find_children(pts_n, "xy") if pts_n else []
        if len(xys) != 2:
            errs.append(f"{sch_path.name}: wire must have 2 xy points")
            continue
        p1 = (float(xys[0][1]), float(xys[0][2]))
        p2 = (float(xys[1][1]), float(xys[1][2]))
        if not all(is_on_grid(c) for c in (*p1, *p2)):
            errs.append(f"{sch_path.name}: off-grid wire ({p1} -> {p2})")
            continue
        wires.append((p1, p2))

    junctions: Set[Tuple[int, int]] = set()
    junction_pts: List[Tuple[float, float]] = []
    for j in find_children(sch, "junction"):
        at_n = find_child(j, "at")
        pt = (float(at_n[1]), float(at_n[2]))
        if not is_on_grid(pt[0]) or not is_on_grid(pt[1]):
            errs.append(f"{sch_path.name}: off-grid junction {pt}")
            continue
        junctions.add(qpt(pt))
        junction_pts.append(pt)

    ncs: Set[Tuple[int, int]] = set()
    nc_pts: List[Tuple[float, float]] = []
    for nc in find_children(sch, "no_connect"):
        at_n = find_child(nc, "at")
        pt = (float(at_n[1]), float(at_n[2]))
        if not is_on_grid(pt[0]) or not is_on_grid(pt[1]):
            errs.append(f"{sch_path.name}: off-grid no_connect {pt}")
            continue
        ncs.add(qpt(pt))
        nc_pts.append(pt)

    labels: List[Tuple[str, Tuple[float, float], int]] = []
    label_qpts: Set[Tuple[int, int]] = set()
    for lb in find_children(sch, "label"):
        at_n = find_child(lb, "at")
        pt = (float(at_n[1]), float(at_n[2]))
        ang = int(float(at_n[3])) if len(at_n) > 3 else 0
        if not is_on_grid(pt[0]) or not is_on_grid(pt[1]):
            errs.append(f"{sch_path.name}: off-grid label {str(lb[1])!r} at {pt}")
            continue
        labels.append((str(lb[1]), pt, ang))
        label_qpts.add(qpt(pt))

    hlabels: List[Tuple[str, str, Tuple[float, float], int]] = []
    hlabel_qpts: Set[Tuple[int, int]] = set()
    for hb in find_children(sch, "hierarchical_label"):
        sh_n = find_child(hb, "shape")
        at_n = find_child(hb, "at")
        pt = (float(at_n[1]), float(at_n[2]))
        ang = int(float(at_n[3])) if len(at_n) > 3 else 0
        if not is_on_grid(pt[0]) or not is_on_grid(pt[1]):
            errs.append(f"{sch_path.name}: off-grid hierarchical_label {str(hb[1])!r} at {pt}")
            continue
        hlabels.append((str(hb[1]), str(sh_n[1]) if sh_n else "passive", pt, ang))
        hlabel_qpts.add(qpt(pt))

    sheets: List[Dict[str, Any]] = []
    sheet_pin_pts: Set[Tuple[int, int]] = set()
    for sb in find_children(sch, "sheet"):
        at_n = find_child(sb, "at")
        sz_n = find_child(sb, "size")
        su_n = find_child(sb, "uuid")
        sx, sy = float(at_n[1]), float(at_n[2])
        sw, sh = float(sz_n[1]), float(sz_n[2])
        su = str(su_n[1]) if su_n else ""
        sname, sfile = "", ""
        for pr in find_children(sb, "property"):
            if pr[1] == "Sheetname":
                sname = str(pr[2])
            elif pr[1] == "Sheetfile":
                sfile = str(pr[2])
        spins = []
        for sp in find_children(sb, "pin"):
            pname = str(sp[1])
            pshape = str(sp[2])
            pat = find_child(sp, "at")
            px, py = float(pat[1]), float(pat[2])
            pang = int(float(pat[3])) if len(pat) > 3 else 0
            spins.append((pname, pshape, (px, py), pang))
            if not is_on_grid(px) or not is_on_grid(py):
                errs.append(f"{sch_path.name}: off-grid sheet pin {pname} at ({px},{py})")
            else:
                sheet_pin_pts.add(qpt((px, py)))
        sheets.append({
            "name": sname, "file": sfile, "uuid": su,
            "x": sx, "y": sy, "w": sw, "h": sh, "pins": spins,
        })

    placed_syms: List[Dict[str, Any]] = []
    pin_pts: List[Tuple[str, str, str, Tuple[float, float]]] = []
    bboxes: List[Tuple[str, float, float, float, float]] = []
    power_out_pins: Dict[Tuple[int, int], List[str]] = defaultdict(list)
    power_net_pts: Dict[str, Set[Tuple[int, int]]] = defaultdict(set)

    for sy_n in find_children(sch, "symbol"):
        lib_id = str(find_child(sy_n, "lib_id")[1])
        at_n = find_child(sy_n, "at")
        un_n = find_child(sy_n, "unit")
        sx, sy = float(at_n[1]), float(at_n[2])
        ang = int(float(at_n[3])) if len(at_n) > 3 else 0
        unit = int(un_n[1]) if un_n else 1

        ref, val = "", ""
        ref_pos, val_pos = (sx, sy), (sx, sy)
        for pr in find_children(sy_n, "property"):
            at_p = find_child(pr, "at")
            if pr[1] == "Reference":
                ref = str(pr[2])
                if at_p:
                    ref_pos = (float(at_p[1]), float(at_p[2]))
            elif pr[1] == "Value":
                val = str(pr[2])
                if at_p:
                    val_pos = (float(at_p[1]), float(at_p[2]))

        if lib_id not in lib_syms:
            errs.append(f"{sch_path.name}: symbol {ref} references missing embedded lib_id {lib_id!r}")
            continue
        if lib_id.startswith("quad_ipkvm:") and project_lib_syms is not None and lib_id.split(":", 1)[1] not in project_lib_syms:
            errs.append(f"{sch_path.name}: symbol {ref} ({lib_id}) missing from quad_ipkvm.kicad_sym")

        ls = lib_syms[lib_id]

        if not ls.is_power:
            if (ref, unit) in seen_ref_units:
                errs.append(f"{sch_path.name}: duplicate Reference+Unit ({ref}, {unit})")
            seen_ref_units.add((ref, unit))

        inst_n = find_child(sy_n, "instances")
        ok_inst = False
        if inst_n:
            for proj_n in find_children(inst_n, "project"):
                for pth_n in find_children(proj_n, "path"):
                    if str(pth_n[1]) == expected_hier_path:
                        ok_inst = True
        if not ok_inst:
            errs.append(f"{sch_path.name}: symbol {ref} missing instance path {expected_hier_path!r}")

        unit_pins = [p for p in ls.pins if p.unit == 0 or p.unit == unit]
        placed_pin_nums = {str(pn[1]) for pn in find_children(sy_n, "pin")}
        expected_pin_nums = {p.num for p in unit_pins}
        if placed_pin_nums != expected_pin_nums:
            errs.append(f"{sch_path.name}: symbol {ref} unit {unit} pin mismatch {placed_pin_nums ^ expected_pin_nums}")

        sym_canvas_pins = []
        for p in unit_pins:
            cp = pin_to_canvas(sx, sy, ang, p.x, p.y)
            if not is_on_grid(cp[0]) or not is_on_grid(cp[1]):
                errs.append(f"{sch_path.name}: off-grid pin {ref}.{p.num} at {cp}")
            else:
                pin_pts.append((ref, p.num, p.etype, cp))
                if p.etype == "power_out":
                    power_out_pins[qpt(cp)].append(f"{ref}.{p.num}")
                if ls.is_power and p.etype == "power_in":
                    power_net_pts[val].add(qpt(cp))
            sym_canvas_pins.append((p, cp))

        # Bounding box across rects, circles, and polylines for active unit
        if not ls.is_power:
            xs: List[float] = []
            ys: List[float] = []
            for ru, x1, y1, x2, y2 in ls.rects:
                if ru in (0, unit):
                    c1 = sym_pt_to_canvas(sx, sy, ang, x1, y1)
                    c2 = sym_pt_to_canvas(sx, sy, ang, x2, y2)
                    xs.extend([c1[0], c2[0]])
                    ys.extend([c1[1], c2[1]])
            for ru, cx0, cy0, rad in ls.circles:
                if ru in (0, unit):
                    cc = sym_pt_to_canvas(sx, sy, ang, cx0, cy0)
                    xs.extend([cc[0] - rad, cc[0] + rad])
                    ys.extend([cc[1] - rad, cc[1] + rad])
            for ru, pts in ls.polys:
                if ru in (0, unit):
                    for lx, ly in pts:
                        cp = sym_pt_to_canvas(sx, sy, ang, lx, ly)
                        xs.append(cp[0])
                        ys.append(cp[1])
            if xs and ys:
                bboxes.append((f"{ref}_u{unit}", min(xs), min(ys), max(xs), max(ys)))

        placed_syms.append({
            "ref": ref, "val": val, "ref_pos": ref_pos, "val_pos": val_pos,
            "lib_id": lib_id, "x": sx, "y": sy,
            "ang": ang, "unit": unit, "ls": ls, "pins": sym_canvas_pins,
        })

    for i in range(len(bboxes)):
        n1, x1a, y1a, x2a, y2a = bboxes[i]
        for j in range(i + 1, len(bboxes)):
            n2, x1b, y1b, x2b, y2b = bboxes[j]
            if x1a < x2b - 0.5 and x2a > x1b + 0.5 and y1a < y2b - 0.5 and y2a > y1b + 0.5:
                errs.append(f"{sch_path.name}: overlapping symbol bodies {n1} and {n2}")
        for sh in sheets:
            if x1a < sh["x"] + sh["w"] - 0.5 and x2a > sh["x"] + 0.5 and y1a < sh["y"] + sh["h"] - 0.5 and y2a > sh["y"] + 0.5:
                errs.append(f"{sch_path.name}: symbol {n1} overlaps sheet {sh['name']}")

    wire_end_deg: Dict[Tuple[int, int], int] = defaultdict(int)
    for a, b in wires:
        wire_end_deg[qpt(a)] += 1
        wire_end_deg[qpt(b)] += 1

    pin_qpts: Dict[Tuple[int, int], List[str]] = defaultdict(list)
    for ref, pnum, _, cp in pin_pts:
        pin_qpts[qpt(cp)].append(f"{ref}.{pnum}")

    all_label_qpts = label_qpts | hlabel_qpts

    for ref, pnum, _, cp in pin_pts:
        qp = qpt(cp)
        has_wire = wire_end_deg[qp] > 0
        has_nc = qp in ncs
        has_lbl = qp in all_label_qpts
        has_peer_pin = len(pin_qpts[qp]) > 1
        if has_nc and (has_wire or has_lbl or has_peer_pin):
            errs.append(f"{sch_path.name}: pin {ref}.{pnum} at {cp} has both no_connect marker and connection")
        if not (has_wire or has_nc or has_lbl or has_peer_pin):
            errs.append(f"{sch_path.name}: floating pin {ref}.{pnum} at {cp}")
        for a, b in wires:
            if pt_on_seg_interior(cp, a, b):
                errs.append(f"{sch_path.name}: pin {ref}.{pnum} touches interior of wire {a}->{b} without split")

    # Check wire endpoints (including degree-1 endpoints hitting another wire interior!)
    checked_qpts: Set[Tuple[int, int]] = set()
    for a, b in wires:
        for ep in (a, b):
            qp = qpt(ep)
            if qp in checked_qpts:
                continue
            checked_qpts.add(qp)
            for sa, sb in wires:
                if pt_on_seg_interior(ep, sa, sb):
                    errs.append(f"{sch_path.name}: wire endpoint {ep} hits interior of wire {sa}->{sb} without split+junction")

    for qp, deg in wire_end_deg.items():
        if deg == 1:
            if qp not in pin_qpts and qp not in all_label_qpts and qp not in sheet_pin_pts:
                pt_mm = (round(qp[0] * GRID, 2), round(qp[1] * GRID, 2))
                errs.append(f"{sch_path.name}: dangling wire endpoint at {pt_mm}")
        elif deg >= 3:
            if qp not in junctions:
                pt_mm = (round(qp[0] * GRID, 2), round(qp[1] * GRID, 2))
                errs.append(f"{sch_path.name}: wire T/cross at {pt_mm} missing explicit (junction)")

    for jpt in junction_pts:
        qp = qpt(jpt)
        if wire_end_deg[qp] < 2:
            errs.append(f"{sch_path.name}: orphan junction at {jpt}")

    texts = []
    for tx in find_children(sch, "text"):
        at_n = find_child(tx, "at")
        eff = find_child(tx, "effects")
        sz = 1.27
        bold = False
        color = (30, 30, 30, 1.0)
        just = "left top"
        if eff:
            fn = find_child(eff, "font")
            if fn:
                szn = find_child(fn, "size")
                if szn:
                    sz = float(szn[1])
                bold = "bold" in fn
                cn = find_child(fn, "color")
                if cn:
                    color = (int(float(cn[1])), int(float(cn[2])), int(float(cn[3])), float(cn[4]))
            jn = find_child(eff, "justify")
            if jn:
                just = " ".join(str(x) for x in jn[1:])
        texts.append((str(tx[1]), float(at_n[1]), float(at_n[2]), sz, bold, color, just))

    rects = []
    for rc in find_children(sch, "rectangle"):
        st = find_child(rc, "start")
        en = find_child(rc, "end")
        stk = find_child(rc, "stroke")
        col = (100, 100, 100, 1.0)
        stype = "solid"
        if stk:
            cn = find_child(stk, "color")
            tn = find_child(stk, "type")
            if cn:
                col = (int(float(cn[1])), int(float(cn[2])), int(float(cn[3])), float(cn[4]))
            if tn:
                stype = str(tn[1])
        rects.append((float(st[1]), float(st[2]), float(en[1]), float(en[2]), col, stype))

    title_n = find_child(sch, "title_block")
    title_str = ""
    if title_n:
        tn = find_child(title_n, "title")
        if tn:
            title_str = str(tn[1])

    return errs, {
        "name": sch_path.name,
        "uuid": sheet_uuid,
        "paper": paper_name,
        "title": title_str,
        "wires": wires,
        "junctions": junction_pts,
        "ncs": nc_pts,
        "labels": labels,
        "hlabels": hlabels,
        "sheets": sheets,
        "symbols": placed_syms,
        "texts": texts,
        "rects": rects,
        "power_out_pins": power_out_pins,
        "power_net_pts": power_net_pts,
        "wire_edges": [(qpt(a), qpt(b)) for a, b in wires],
    }


# ---------------------------------------------------------------------------
# SVG Renderer (Dynamic A2/A3 + Square ViewBox Option for qlmanage)
# ---------------------------------------------------------------------------

def render_svg(data: Dict[str, Any], out_svg: Path) -> None:
    pw, ph = PAPER_SIZES.get(data.get("paper", "A2"), (594.0, 420.0))
    # Pad vertically into a square viewBox so macOS qlmanage renders the full width uncropped!
    vmax = max(pw, ph)
    vy_off = round((vmax - ph) / 2.0, 2)
    tb_x, tb_y, tb_w, tb_h = pw - 128.0, ph - 43.0, 122.0, 37.0

    s: List[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="2800" height="2800" viewBox="0 {-vy_off} {vmax} {vmax}">',
        f'<rect x="0" y="0" width="{pw}" height="{ph}" fill="#fdfcf7"/>',
        f'<rect x="6" y="6" width="{pw-12}" height="{ph-12}" fill="none" stroke="#334155" stroke-width="0.45"/>',
        f'<rect x="{tb_x}" y="{tb_y}" width="{tb_w}" height="{tb_h}" fill="#f8fafc" stroke="#334155" stroke-width="0.35"/>',
        f'<text x="{tb_x+3}" y="{tb_y+7}" font-family="Helvetica, Arial, sans-serif" font-size="3.1" font-weight="bold" fill="#0f172a">{html.escape(data["title"][:48])}</text>',
        f'<text x="{tb_x+3}" y="{tb_y+14}" font-family="Helvetica, Arial, sans-serif" font-size="2.3" fill="#475569">File: {html.escape(data["name"])}  |  Format: KiCad 8 (20231120) {html.escape(data["paper"])}</text>',
        f'<text x="{tb_x+3}" y="{tb_y+21}" font-family="Helvetica, Arial, sans-serif" font-size="2.3" fill="#475569">Project: quad-ipkvm  |  Rev: 1.0  |  Date: 2026-11-27</text>',
        f'<text x="{tb_x+3}" y="{tb_y+28}" font-family="Helvetica, Arial, sans-serif" font-size="2.1" fill="#64748b">Primary ref: docs/WIRING.md (verify ERC in KiCad GUI)</text>',
    ]

    for x1, y1, x2, y2, col, stype in data["rects"]:
        c_hex = f"rgb({col[0]},{col[1]},{col[2]})"
        dash = ' stroke-dasharray="2.0,1.2"' if stype == "dash" else ""
        op = "0.2" if stype == "dash" else "0.8"
        s.append(f'<rect x="{min(x1,x2)}" y="{min(y1,y2)}" width="{abs(x2-x1)}" height="{abs(y2-y1)}" fill="#ffffff" fill-opacity="{op}" stroke="{c_hex}" stroke-width="0.32"{dash}/>')

    for (x1, y1), (x2, y2) in data["wires"]:
        s.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="#15803d" stroke-width="0.3"/>')

    for sym in data["symbols"]:
        sx, sy, ang, unit = sym["x"], sym["y"], sym["ang"], sym["unit"]
        ls: ParsedLibSym = sym["ls"]
        fill_col = "none" if ls.is_power else "#fefce8"
        stk_col = "#15803d" if ls.is_power else "#991b1b"
        for ru, rx1, ry1, rx2, ry2 in ls.rects:
            if ru in (0, unit):
                c1 = sym_pt_to_canvas(sx, sy, ang, rx1, ry1)
                c2 = sym_pt_to_canvas(sx, sy, ang, rx2, ry2)
                s.append(f'<rect x="{min(c1[0],c2[0])}" y="{min(c1[1],c2[1])}" width="{abs(c2[0]-c1[0])}" height="{abs(c2[1]-c1[1])}" fill="{fill_col}" stroke="{stk_col}" stroke-width="0.28"/>')
        for ru, pts in ls.polys:
            if ru in (0, unit) and len(pts) >= 2:
                cpts = [sym_pt_to_canvas(sx, sy, ang, px, py) for px, py in pts]
                pstr = " ".join(f"{cx},{cy}" for cx, cy in cpts)
                s.append(f'<polyline points="{pstr}" fill="none" stroke="{stk_col}" stroke-width="0.26"/>')
        for ru, cx0, cy0, rad in ls.circles:
            if ru in (0, unit):
                cc = sym_pt_to_canvas(sx, sy, ang, cx0, cy0)
                s.append(f'<circle cx="{cc[0]}" cy="{cc[1]}" r="{rad}" fill="none" stroke="{stk_col}" stroke-width="0.26"/>')

        for p, cp in sym["pins"]:
            pa_total = (p.angle + ang) % 360
            dx, dy = rotate_ccw(p.length, 0.0, pa_total)
            bx, by = cp[0] + dx, cp[1] - dy
            if p.length > 0.01:
                s.append(f'<line x1="{cp[0]}" y1="{cp[1]}" x2="{bx}" y2="{by}" stroke="#991b1b" stroke-width="0.24"/>')
            if not p.hide and p.name != "~":
                inx, iny = rotate_ccw(p.length + 0.8, 0.0, pa_total)
                tx, ty = cp[0] + inx, cp[1] - iny
                anchor = "start" if dx > 0.1 else ("end" if dx < -0.1 else "middle")
                s.append(f'<text x="{tx}" y="{ty+0.5}" text-anchor="{anchor}" font-family="Helvetica, Arial, sans-serif" font-size="1.55" fill="#0f766e">{html.escape(p.name)}</text>')
                midx, midy = (cp[0] + bx) / 2.0, (cp[1] + by) / 2.0
                if abs(dx) >= abs(dy):
                    s.append(f'<text x="{midx}" y="{midy-0.4}" text-anchor="middle" font-family="Helvetica, Arial, sans-serif" font-size="1.35" fill="#7f1d1d">{html.escape(p.num)}</text>')
                else:
                    s.append(f'<text x="{midx+0.8}" y="{midy+0.4}" text-anchor="start" font-family="Helvetica, Arial, sans-serif" font-size="1.35" fill="#7f1d1d">{html.escape(p.num)}</text>')

        if ls.is_power:
            val_y = sy - 3.2 if sym["val"] in ("+3V3", "PWR_FLAG") else sy + 4.3
            s.append(f'<text x="{sx}" y="{val_y}" text-anchor="middle" font-family="Helvetica, Arial, sans-serif" font-size="1.8" font-weight="bold" fill="#0f766e">{html.escape(sym["val"])}</text>')
        else:
            rx, ry = sym["ref_pos"]
            vx, vy = sym["val_pos"]
            s.append(f'<text x="{rx}" y="{ry}" text-anchor="middle" font-family="Helvetica, Arial, sans-serif" font-size="1.8" font-weight="bold" fill="#0f766e">{html.escape(sym["ref"])}</text>')
            s.append(f'<text x="{vx}" y="{vy+0.6}" text-anchor="middle" font-family="Helvetica, Arial, sans-serif" font-size="1.6" fill="#1e293b">{html.escape(sym["val"])}</text>')

    for jx, jy in data["junctions"]:
        s.append(f'<circle cx="{jx}" cy="{jy}" r="0.55" fill="#15803d"/>')

    for nx, ny in data["ncs"]:
        d = 0.75
        s.append(f'<line x1="{nx-d}" y1="{ny-d}" x2="{nx+d}" y2="{ny+d}" stroke="#2563eb" stroke-width="0.28"/>')
        s.append(f'<line x1="{nx-d}" y1="{ny+d}" x2="{nx+d}" y2="{ny-d}" stroke="#2563eb" stroke-width="0.28"/>')

    for name, (lx, ly), lang in data["labels"]:
        anchor = "end" if lang == 180 else "start"
        tx = lx - 0.5 if lang == 180 else lx + 0.5
        s.append(f'<text x="{tx}" y="{ly-0.5}" text-anchor="{anchor}" font-family="Helvetica, Arial, sans-serif" font-size="1.7" font-weight="bold" fill="#111827">{html.escape(name)}</text>')

    for txt, tx, ty, sz, bold, col, _ in data["texts"]:
        c_hex = f"rgb({col[0]},{col[1]},{col[2]})"
        fw = "bold" if bold else "normal"
        fs = round(sz * 1.38, 2)
        for idx, line in enumerate(txt.split("\n")):
            ly = round(ty + idx * (fs * 1.38), 2)
            s.append(f'<text x="{tx}" y="{ly}" font-family="Helvetica, Arial, sans-serif" font-size="{fs}" font-weight="{fw}" fill="{c_hex}">{html.escape(line)}</text>')

    s.append("</svg>")
    out_svg.write_text("\n".join(s), encoding="utf-8")


# ---------------------------------------------------------------------------
# Project-wide Verification Driver
# ---------------------------------------------------------------------------

def verify_project(pro_path: Path, render_dir: Optional[Path]) -> int:
    errs: List[str] = []
    if not pro_path.exists():
        print(f"ERROR: Project file not found: {pro_path}", file=sys.stderr)
        return 1

    pro_data = json.loads(pro_path.read_text(encoding="utf-8"))
    if pro_data.get("meta", {}).get("filename") != pro_path.name:
        errs.append(".kicad_pro meta.filename mismatch")

    # Verify sym-lib-table + project symbol library quad_ipkvm.kicad_sym
    sym_table_path = pro_path.parent / "sym-lib-table"
    sym_lib_path = pro_path.parent / "quad_ipkvm.kicad_sym"
    project_lib_syms: Optional[Set[str]] = None

    if not sym_table_path.exists():
        errs.append("Missing sym-lib-table in project directory")
    else:
        try:
            tbl_ast = parse_sexpr(sym_table_path.read_text(encoding="utf-8"))
            if not tbl_ast or not isinstance(tbl_ast[0], list) or not tbl_ast[0] or tbl_ast[0][0] != "sym_lib_table":
                errs.append("Invalid sym-lib-table format")
            else:
                found_proj_lib = False
                for lib_n in find_children(tbl_ast[0], "lib"):
                    name_n = find_child(lib_n, "name")
                    uri_n = find_child(lib_n, "uri")
                    if name_n and len(name_n) >= 2 and str(name_n[1]) == "quad_ipkvm":
                        found_proj_lib = True
                        uri_str = str(uri_n[1]) if uri_n and len(uri_n) >= 2 else ""
                        resolved_uri = Path(uri_str.replace("${KIPRJMOD}", str(pro_path.parent))).resolve()
                        if resolved_uri != sym_lib_path.resolve():
                            errs.append(
                                f"sym-lib-table 'quad_ipkvm' uri {uri_str!r} does not resolve to {sym_lib_path}"
                            )
                if not found_proj_lib:
                    errs.append("sym-lib-table missing library entry named 'quad_ipkvm'")
        except ValueError as exc:
            errs.append(f"sym-lib-table parse error: {exc}")

    if not sym_lib_path.exists():
        errs.append("Missing quad_ipkvm.kicad_sym in project directory")
    else:
        try:
            sym_ast = parse_sexpr(sym_lib_path.read_text(encoding="utf-8"))
            if not sym_ast or not isinstance(sym_ast[0], list) or not sym_ast[0] or sym_ast[0][0] != "kicad_symbol_lib":
                errs.append("Invalid quad_ipkvm.kicad_sym top-level node")
            else:
                project_lib_syms = set()
                for snode in find_children(sym_ast[0], "symbol"):
                    ls, lerrs = parse_lib_symbol(snode, require_prefix=False)
                    errs.extend([f"quad_ipkvm.kicad_sym: {e}" for e in lerrs])
                    project_lib_syms.add(ls.lib_id)
        except ValueError as exc:
            errs.append(f"quad_ipkvm.kicad_sym parse error: {exc}")

    root_sch = pro_path.with_suffix(".kicad_sch")
    if not root_sch.exists():
        errs.append(f"Missing root schematic {root_sch.name}")
        for e in errs:
            print(f"  - {e}", file=sys.stderr)
        return 1

    try:
        root_ast_list = parse_sexpr(root_sch.read_text(encoding="utf-8"))
    except ValueError as exc:
        print(f"FAILED: {root_sch.name} parse error: {exc}", file=sys.stderr)
        return 1

    if not root_ast_list or not isinstance(root_ast_list[0], list) or not root_ast_list[0] or root_ast_list[0][0] != "kicad_sch":
        print(f"FAILED: {root_sch.name} missing (kicad_sch ...)", file=sys.stderr)
        return 1

    uuid_node = find_child(root_ast_list[0], "uuid")
    if not uuid_node or len(uuid_node) < 2:
        print(f"FAILED: {root_sch.name} missing root uuid", file=sys.stderr)
        return 1
    root_uuid = str(uuid_node[1])

    seen_uuids: Dict[str, str] = {}
    seen_ref_units: Set[Tuple[str, int]] = set()
    all_sheets_data: List[Dict[str, Any]] = []
    loaded_children: List[Tuple[Dict[str, Any], Dict[str, Any]]] = []

    root_errs, root_data = validate_sheet(root_sch, f"/{root_uuid}", True, seen_uuids, seen_ref_units, project_lib_syms)
    errs.extend(root_errs)
    if root_data is not None:
        all_sheets_data.append(root_data)

        # Traverse any child hierarchical sheets if present
        for sh in root_data["sheets"]:
            child_path = pro_path.parent / sh["file"]
            if not child_path.exists():
                errs.append(f"Missing child sheet file {sh['file']} referenced on root")
                continue
            c_hier = f"/{root_uuid}/{sh['uuid']}"
            c_errs, c_data = validate_sheet(child_path, c_hier, False, seen_uuids, seen_ref_units, project_lib_syms)
            errs.extend(c_errs)
            if c_data is None:
                continue
            all_sheets_data.append(c_data)
            loaded_children.append((sh, c_data))

            sp_counts = Counter((pname, pshape) for pname, pshape, _, _ in sh["pins"])
            hl_counts = Counter((hname, hshape) for hname, hshape, _, _ in c_data["hlabels"])
            if sp_counts != hl_counts:
                errs.append(f"{sh['file']}: sheet pin <-> hierarchical_label mismatch: root={dict(sp_counts)} child={dict(hl_counts)}")

        # Project-wide power rail ERC check across wires, same-sheet local labels, and hierarchical pins
        adj: Dict[Tuple[Any, ...], Set[Tuple[Any, ...]]] = defaultdict(set)
        power_out_pins_by_node: Dict[Tuple[Any, ...], Set[Tuple[str, str]]] = defaultdict(set)

        for d in all_sheets_data:
            sn = d["name"]
            for a, b in d["wire_edges"]:
                na, nb = (sn, *a), (sn, *b)
                adj[na].add(nb)
                adj[nb].add(na)
            for p_qpt, pin_ids in d["power_out_pins"].items():
                for pid in pin_ids:
                    power_out_pins_by_node[(sn, *p_qpt)].add((sn, pid))
            # Bridge same-name local labels within the SAME sheet (never across sheets!)
            for lname, lpt_mm, _ in d["labels"]:
                lnode = (sn, *qpt(lpt_mm))
                lbl_hub = ("__LBL__", sn, lname)
                adj[lnode].add(lbl_hub)
                adj[lbl_hub].add(lnode)
                # If label sits in interior of any wire on this sheet, attach it to that wire's net
                for wa, wb in d["wires"]:
                    if pt_on_seg_interior(lpt_mm, wa, wb):
                        wnode = (sn, *qpt(wa))
                        adj[lnode].add(wnode)
                        adj[wnode].add(lnode)
            # Bridge same-name power symbols globally (as KiCad global power nets do)
            for pnet, pts in d["power_net_pts"].items():
                for pt in pts:
                    gnode = ("__PWR__", pnet)
                    npt = (sn, *pt)
                    adj[npt].add(gnode)
                    adj[gnode].add(npt)

        # Bridge hierarchical sheet pins <-> child hierarchical labels using loaded child association
        for sh, cdata in loaded_children:
            for pname, _, pt, _ in sh["pins"]:
                for hname, _, hpt, _ in cdata["hlabels"]:
                    if pname == hname:
                        na = (root_data["name"], *qpt(pt))
                        nb = (cdata["name"], *qpt(hpt))
                        adj[na].add(nb)
                        adj[nb].add(na)
                        # If hierarchical label sits on wire interior in child, attach it
                        for wa, wb in cdata["wires"]:
                            if pt_on_seg_interior(hpt, wa, wb):
                                wnode = (cdata["name"], *qpt(wa))
                                adj[nb].add(wnode)
                                adj[wnode].add(nb)

        for pnet in ("+3V3", "GND"):
            start = ("__PWR__", pnet)
            vis: Set[Tuple[Any, ...]] = {start}
            q = [start]
            drivers: Set[Tuple[str, str]] = set()
            while q:
                cur = q.pop()
                drivers.update(power_out_pins_by_node.get(cur, ()))
                for nb in adj[cur]:
                    if nb not in vis:
                        vis.add(nb)
                        q.append(nb)
            if len(drivers) == 0:
                errs.append(f"Power net {pnet} has no power_out driver (component power_out or PWR_FLAG)")
            elif len(drivers) > 1:
                errs.append(f"Power net {pnet} has multiple conflicting power_out drivers: {sorted(drivers)}")

    if render_dir is not None and all_sheets_data:
        render_dir.mkdir(parents=True, exist_ok=True)
        for d in all_sheets_data:
            out_svg = render_dir / (Path(d["name"]).stem + ".svg")
            render_svg(d, out_svg)
            print(f"Rendered SVG: {out_svg}")

    if errs:
        print(f"FAILED with {len(errs)} error(s):", file=sys.stderr)
        for e in errs:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"PASS: Validated {len(all_sheets_data)} KiCad sheet(s), {len(seen_uuids)} unique UUIDs, 0 errors.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate KiCad 8/9 schematic project & optionally render SVG previews")
    ap.add_argument("pro", type=Path, help="Path to .kicad_pro file")
    ap.add_argument("--render-svg", type=Path, default=None, help="Output directory for SVG sheet previews")
    args = ap.parse_args()
    return verify_project(args.pro, args.render_svg)


if __name__ == "__main__":
    raise SystemExit(main())
