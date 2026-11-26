#!/usr/bin/env python3
"""
Deterministic KiCad 8/9 hierarchical schematic generator for quad-ipkvm.

Generates hardware/kicad/quad-ipkvm.kicad_pro and 5 hierarchical .kicad_sch files
based on docs/WIRING.md (including PR #174 owner decisions: R1=130k/R2=162k 0.5% E96
VBUS divider and no J3 ESD diode).

KiCad coordinate conventions enforced:
- Inside lib_symbols: Y-UP coordinate space.
- On schematic canvas: Y-DOWN coordinate space (origin top-left).
- Canvas position of pin (px, py) on symbol at (sx, sy, angle):
    rotate (px, py) CCW by angle -> (rx, ry), then (sx + rx, sy - ry).
"""

from __future__ import annotations

import json
import sys
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

KICAD_VERSION = 20231120
GENERATOR_NAME = "quad_ipkvm_gen"
PROJECT_NAME = "quad-ipkvm"
GRID = 1.27  # 50 mil standard KiCad schematic grid (mm)
NS_UUID = uuid.UUID("7f39b410-9e2a-4c61-8a14-51a9d802c001")


def g(units: int) -> float:
    """Return exact 1.27 mm grid-snapped coordinate for integer grid units."""
    return round(units * GRID, 2)


def fmt(v: float) -> str:
    """Format float compactly without trailing zeros."""
    r = round(float(v), 4)
    if abs(r - round(r)) < 1e-6:
        return str(int(round(r)))
    s = f"{r:.4f}".rstrip("0").rstrip(".")
    return s if s != "-0" else "0"


def q(s: str) -> str:
    """Quote string for KiCad S-expression."""
    escaped = s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return f'"{escaped}"'


def det_uuid(key: str) -> str:
    """Deterministic UUIDv5 from unique hierarchical key."""
    return str(uuid.uuid5(NS_UUID, key))


def rotate_ccw(px: float, py: float, angle: int) -> Tuple[float, float]:
    """Rotate symbol-local coordinates (Y-up) CCW by 0/90/180/270 degrees."""
    a = angle % 360
    if a == 0:
        return (px, py)
    if a == 90:
        return (-py, px)
    if a == 180:
        return (-px, -py)
    if a == 270:
        return (py, -px)
    raise ValueError(f"Unsupported orthogonal angle: {angle}")


def pin_canvas_pos(sx: float, sy: float, angle: int, px: float, py: float) -> Tuple[float, float]:
    """Map symbol-local Y-up pin tip (px, py) to schematic Y-down canvas coordinates."""
    rx, ry = rotate_ccw(px, py, angle)
    return (round(sx + rx, 2), round(sy - ry, 2))


@dataclass
class PinDef:
    number: str
    name: str
    etype: str
    x: float
    y: float
    angle: int
    length: float = 2.54
    unit: int = 1
    hide: bool = False


@dataclass
class LibSymDef:
    lib_id: str
    ref_prefix: str
    value: str
    footprint: str = ""
    is_power: bool = False
    in_bom: bool = True
    on_board: bool = True
    pin_names_offset: float = 1.016
    hide_pin_names: bool = False
    hide_pin_numbers: bool = False
    unit_count: int = 1
    ref_pos: Tuple[float, float, int] = (0.0, 3.81, 0)
    val_pos: Tuple[float, float, int] = (0.0, -3.81, 0)
    graphics_by_unit: Dict[int, List[str]] = field(default_factory=dict)
    pins: List[PinDef] = field(default_factory=list)

    @property
    def short_name(self) -> str:
        return self.lib_id.split(":", 1)[1]

    def to_sexpr(self) -> str:
        lines: List[str] = []
        pn_hide = " hide" if self.hide_pin_names else ""
        pnum_hide = " (pin_numbers hide)" if self.hide_pin_numbers else ""
        pwr = " (power)" if self.is_power else ""
        in_bom = "yes" if self.in_bom else "no"
        on_board = "yes" if self.on_board else "no"
        ref_hide = " hide" if self.is_power else ""
        lines.append(
            f'    (symbol {q(self.lib_id)}{pwr}{pnum_hide}'
            f' (pin_names (offset {fmt(self.pin_names_offset)}){pn_hide})'
            f' (exclude_from_sim no) (in_bom {in_bom}) (on_board {on_board})'
        )
        rx, ry, ra = self.ref_pos
        vx, vy, va = self.val_pos
        lines.append(
            f'      (property "Reference" {q(self.ref_prefix)} (at {fmt(rx)} {fmt(ry)} {ra})'
            f' (effects (font (size 1.27 1.27)){ref_hide}))'
        )
        lines.append(
            f'      (property "Value" {q(self.value)} (at {fmt(vx)} {fmt(vy)} {va})'
            f' (effects (font (size 1.27 1.27))))'
        )
        lines.append(
            f'      (property "Footprint" {q(self.footprint)} (at 0 0 0)'
            f' (effects (font (size 1.27 1.27)) hide))'
        )
        lines.append(
            f'      (property "Datasheet" "~" (at 0 0 0)'
            f' (effects (font (size 1.27 1.27)) hide))'
        )

        g0 = self.graphics_by_unit.get(0, [])
        if g0:
            lines.append(f'      (symbol {q(f"{self.short_name}_0_1")}')
            for item in g0:
                lines.append(f"        {item}")
            lines.append("      )")

        for u in range(1, self.unit_count + 1):
            gu = self.graphics_by_unit.get(u, [])
            pu = [p for p in self.pins if p.unit == u]
            if not gu and not pu:
                continue
            lines.append(f'      (symbol {q(f"{self.short_name}_{u}_1")}')
            for item in gu:
                lines.append(f"        {item}")
            for p in pu:
                phide = " hide" if p.hide else ""
                lines.append(
                    f'        (pin {p.etype} line (at {fmt(p.x)} {fmt(p.y)} {p.angle})'
                    f' (length {fmt(p.length)}){phide}'
                    f' (name {q(p.name)} (effects (font (size 1.016 1.016))))'
                    f' (number {q(p.number)} (effects (font (size 1.016 1.016))))'
                    f")"
                )
            lines.append("      )")
        lines.append("    )")
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# Symbol Library Definitions (all grid-exact 1.27 mm)
# ---------------------------------------------------------------------------

def make_power_3v3() -> LibSymDef:
    return LibSymDef(
        lib_id="power:+3V3",
        ref_prefix="#PWR",
        value="+3V3",
        is_power=True,
        in_bom=False,
        on_board=False,
        pin_names_offset=0.0,
        hide_pin_names=True,
        hide_pin_numbers=True,
        ref_pos=(0.0, -2.54, 0),
        val_pos=(0.0, 3.81, 0),
        graphics_by_unit={
            0: [
                "(polyline (pts (xy -0.762 1.27) (xy 0 2.54) (xy 0.762 1.27)) (stroke (width 0) (type default)) (fill (type none)))",
                "(polyline (pts (xy 0 0) (xy 0 2.54)) (stroke (width 0) (type default)) (fill (type none)))",
            ]
        },
        pins=[PinDef("1", "+3V3", "power_in", 0.0, 0.0, 90, length=0.0, hide=True)],
    )


def make_power_gnd() -> LibSymDef:
    return LibSymDef(
        lib_id="power:GND",
        ref_prefix="#PWR",
        value="GND",
        is_power=True,
        in_bom=False,
        on_board=False,
        pin_names_offset=0.0,
        hide_pin_names=True,
        hide_pin_numbers=True,
        ref_pos=(0.0, 2.54, 0),
        val_pos=(0.0, -3.81, 0),
        graphics_by_unit={
            0: [
                "(polyline (pts (xy 0 0) (xy 0 -1.27) (xy 1.27 -1.27) (xy 0 -2.54) (xy -1.27 -1.27) (xy 0 -1.27)) (stroke (width 0) (type default)) (fill (type none)))",
            ]
        },
        pins=[PinDef("1", "GND", "power_in", 0.0, 0.0, 270, length=0.0, hide=True)],
    )


def make_power_flag() -> LibSymDef:
    return LibSymDef(
        lib_id="power:PWR_FLAG",
        ref_prefix="#FLG",
        value="PWR_FLAG",
        is_power=True,
        in_bom=False,
        on_board=False,
        pin_names_offset=0.0,
        hide_pin_names=True,
        hide_pin_numbers=True,
        ref_pos=(0.0, -2.54, 0),
        val_pos=(0.0, 3.81, 0),
        graphics_by_unit={
            0: [
                "(polyline (pts (xy 0 0) (xy 0 1.27) (xy -1.016 1.905) (xy 0 2.54) (xy 1.016 1.905) (xy 0 1.27)) (stroke (width 0) (type default)) (fill (type none)))",
            ]
        },
        pins=[PinDef("1", "pwr", "power_out", 0.0, 0.0, 90, length=0.0, hide=True)],
    )


def make_resistor() -> LibSymDef:
    return LibSymDef(
        lib_id="Device:R",
        ref_prefix="R",
        value="R",
        footprint="Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal",
        pin_names_offset=0.0,
        hide_pin_names=True,
        hide_pin_numbers=True,
        ref_pos=(2.54, 1.27, 0),
        val_pos=(2.54, -1.27, 0),
        graphics_by_unit={
            0: [
                "(rectangle (start -1.016 2.54) (end 1.016 -2.54) (stroke (width 0.254) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("1", "~", "passive", 0.0, g(3), 270, length=1.27),
            PinDef("2", "~", "passive", 0.0, g(-3), 90, length=1.27),
        ],
    )


def make_cap() -> LibSymDef:
    return LibSymDef(
        lib_id="Device:C",
        ref_prefix="C",
        value="100nF",
        footprint="Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm",
        pin_names_offset=0.254,
        hide_pin_names=True,
        hide_pin_numbers=True,
        ref_pos=(2.54, 1.27, 0),
        val_pos=(2.54, -1.27, 0),
        graphics_by_unit={
            0: [
                "(polyline (pts (xy -2.032 0.762) (xy 2.032 0.762)) (stroke (width 0.3556) (type default)) (fill (type none)))",
                "(polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762)) (stroke (width 0.3556) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("1", "~", "passive", 0.0, g(3), 270, length=3.048),
            PinDef("2", "~", "passive", 0.0, g(-3), 90, length=3.048),
        ],
    )


def make_led() -> LibSymDef:
    return LibSymDef(
        lib_id="Device:LED",
        ref_prefix="D",
        value="LED",
        footprint="LED_THT:LED_D3.0mm",
        pin_names_offset=1.016,
        hide_pin_names=True,
        ref_pos=(0.0, 3.81, 0),
        val_pos=(0.0, -3.81, 0),
        graphics_by_unit={
            0: [
                "(polyline (pts (xy -1.27 -1.27) (xy -1.27 1.27)) (stroke (width 0.254) (type default)) (fill (type none)))",
                "(polyline (pts (xy 1.27 -1.27) (xy 1.27 1.27) (xy -1.27 0) (xy 1.27 -1.27)) (stroke (width 0.254) (type default)) (fill (type none)))",
                "(polyline (pts (xy -0.508 1.905) (xy -1.778 3.175)) (stroke (width 0.1524) (type default)) (fill (type none)))",
                "(polyline (pts (xy 0.508 1.905) (xy -0.762 3.175)) (stroke (width 0.1524) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("2", "A", "passive", g(3), 0.0, 180, length=2.54),
            PinDef("1", "K", "passive", g(-3), 0.0, 0, length=2.54),
        ],
    )


def make_switch_push() -> LibSymDef:
    return LibSymDef(
        lib_id="Switch:SW_Push",
        ref_prefix="SW",
        value="SW_Push",
        footprint="Button_Switch_THT:SW_PUSH_6mm",
        pin_names_offset=1.016,
        hide_pin_names=True,
        ref_pos=(0.0, 3.81, 0),
        val_pos=(0.0, -2.54, 0),
        graphics_by_unit={
            0: [
                "(circle (center -2.032 0) (radius 0.508) (stroke (width 0.254) (type default)) (fill (type none)))",
                "(circle (center 2.032 0) (radius 0.508) (stroke (width 0.254) (type default)) (fill (type none)))",
                "(polyline (pts (xy -2.032 1.27) (xy 2.032 1.27)) (stroke (width 0.254) (type default)) (fill (type none)))",
                "(polyline (pts (xy 0 1.27) (xy 0 2.54)) (stroke (width 0.254) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("1", "1", "passive", g(-4), 0.0, 0, length=2.54),
            PinDef("2", "2", "passive", g(4), 0.0, 180, length=2.54),
        ],
    )


def make_conn_1xN(
    lib_id: str,
    n: int,
    pin_names: List[str],
    pin_types: Optional[List[str]] = None,
    width_units: int = 14,
    spacing_units: int = 4,
    footprint: str = "",
) -> LibSymDef:
    """Create a 1xN connector with pins exiting RIGHT (angle=180), all coordinates on exact 1.27 mm grid."""
    assert len(pin_names) == n
    total_units = (n - 1) * spacing_units
    top_u = total_units // 2
    box_top = g(top_u + 2)
    box_bot = g(top_u - total_units - 2)
    half_w = g(width_units // 2)
    pin_x = g(width_units // 2 + 4)

    pins: List[PinDef] = []
    for i in range(n):
        py = g(top_u - i * spacing_units)
        pt = pin_types[i] if pin_types else "passive"
        pins.append(PinDef(str(i + 1), pin_names[i], pt, pin_x, py, 180, length=g(4)))

    return LibSymDef(
        lib_id=lib_id,
        ref_prefix="J",
        value=lib_id.split(":", 1)[1],
        footprint=footprint,
        pin_names_offset=1.016,
        ref_pos=(0.0, round(box_top + g(2), 2), 0),
        val_pos=(0.0, round(box_bot - g(2), 2), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-half_w)} {fmt(box_top)}) (end {fmt(half_w)} {fmt(box_bot)})"
                " (stroke (width 0.254) (type default)) (fill (type background)))",
            ]
        },
        pins=pins,
    )


def make_conn_1xN_left(
    lib_id: str,
    n: int,
    pin_names: List[str],
    pin_types: Optional[List[str]] = None,
    width_units: int = 14,
    spacing_units: int = 4,
    footprint: str = "",
) -> LibSymDef:
    """Create a 1xN connector with pins exiting LEFT (angle=0), all coordinates on exact 1.27 mm grid."""
    assert len(pin_names) == n
    total_units = (n - 1) * spacing_units
    top_u = total_units // 2
    box_top = g(top_u + 2)
    box_bot = g(top_u - total_units - 2)
    half_w = g(width_units // 2)
    pin_x = g(-(width_units // 2 + 4))

    pins = [
        PinDef(
            str(i + 1),
            pin_names[i],
            pin_types[i] if pin_types else "passive",
            pin_x,
            g(top_u - i * spacing_units),
            0,
            length=g(4),
        )
        for i in range(n)
    ]
    return LibSymDef(
        lib_id=lib_id,
        ref_prefix="J",
        value=lib_id.split(":", 1)[1],
        footprint=footprint,
        pin_names_offset=1.016,
        ref_pos=(0.0, round(box_top + g(2), 2), 0),
        val_pos=(0.0, round(box_bot - g(2), 2), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-half_w)} {fmt(box_top)}) (end {fmt(half_w)} {fmt(box_bot)})"
                " (stroke (width 0.254) (type default)) (fill (type background)))",
            ]
        },
        pins=pins,
    )


def make_p1_40pin() -> LibSymDef:
    """Waveshare ESP32-P4-ETH P1 (PICO-2x20 IO) matching docs/WIRING.md §4 exact pinout."""
    left_specs = [
        (1, "54/GPIO54", "bidirectional"),
        (2, "19/GPIO19", "bidirectional"),
        (3, "GND", "power_in"),
        (4, "18/GPIO18", "bidirectional"),
        (5, "17/GPIO17", "bidirectional"),
        (6, "16/GPIO16", "bidirectional"),
        (7, "15/GPIO15", "bidirectional"),
        (8, "GND", "power_in"),
        (9, "14/GPIO14", "bidirectional"),
        (10, "6/GPIO6", "bidirectional"),
        (11, "5/GPIO5(JTAG)", "bidirectional"),
        (12, "4/GPIO4(JTAG)", "bidirectional"),
        (13, "GND", "power_in"),
        (14, "3/GPIO3(JTAG)", "bidirectional"),
        (15, "2/GPIO2(JTAG)", "bidirectional"),
        (16, "SCL/GPIO8", "bidirectional"),
        (17, "SDA/GPIO7", "bidirectional"),
        (18, "GND", "power_in"),
        (19, "24/USB1P1_N", "passive"),
        (20, "25/USB1P1_P", "passive"),
    ]
    right_specs = [
        (40, "VBUS/VCC1_5V", "passive"),
        (39, "VSYS/VCC_5V", "passive"),
        (38, "GND", "power_in"),
        (37, "EN/3V3_EN", "passive"),
        (36, "3V3/ESP_3V3", "power_out"),
        (35, "20/GPIO20", "bidirectional"),
        (34, "21/GPIO21", "bidirectional"),
        (33, "GND", "power_in"),
        (32, "22/GPIO22", "bidirectional"),
        (31, "23/GPIO23", "bidirectional"),
        (30, "RUN/ESP_EN", "passive"),
        (29, "26/USB_FS_N", "passive"),
        (28, "GND", "power_in"),
        (27, "27/USB_FS_P", "passive"),
        (26, "32/GPIO32(IN)", "input"),
        (25, "33/GPIO33(IN)", "input"),
        (24, "46/GPIO46", "bidirectional"),
        (23, "GND", "power_in"),
        (22, "47/GPIO47", "bidirectional"),
        (21, "48/GPIO48", "bidirectional"),
    ]
    spacing = g(4)  # 5.08 mm
    top_y = g(38)
    half_w = g(16)
    box_top = round(top_y + g(3), 2)
    box_bot = round(-top_y - g(3), 2)
    pin_len = g(4)
    lx = round(-half_w - pin_len, 2)
    rx = round(half_w + pin_len, 2)

    pins: List[PinDef] = []
    for idx, (num, name, etype) in enumerate(left_specs):
        pins.append(PinDef(str(num), name, etype, lx, round(top_y - idx * spacing, 2), 0, length=pin_len))
    for idx, (num, name, etype) in enumerate(right_specs):
        pins.append(PinDef(str(num), name, etype, rx, round(top_y - idx * spacing, 2), 180, length=pin_len))

    return LibSymDef(
        lib_id="quad_ipkvm:ESP32-P4-ETH_P1",
        ref_prefix="P",
        value="ESP32-P4-ETH_P1_2x20",
        footprint="Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Vertical",
        pin_names_offset=1.016,
        ref_pos=(0.0, round(box_top + g(2), 2), 0),
        val_pos=(0.0, round(box_bot - g(2), 2), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-half_w)} {fmt(box_top)}) (end {fmt(half_w)} {fmt(box_bot)})"
                " (stroke (width 0.3048) (type default)) (fill (type background)))",
                f'(polyline (pts (xy 0 {fmt(box_top)}) (xy 0 {fmt(box_bot)})) (stroke (width 0.1524) (type dash)) (fill (type none)))',
            ]
        },
        pins=pins,
    )


def make_mcp23017_module() -> LibSymDef:
    """Switch Science SSCI-016506 MCP23017 3.3V module with generous vertical spacing for outputs/switches."""
    half_w = g(12)
    pin_len = g(4)
    lx = round(-half_w - pin_len, 2)
    rx = round(half_w + pin_len, 2)
    box_top = g(36)
    box_bot = g(-36)

    left_pins = [
        ("1", "VDD", "power_in", g(32)),
        ("2", "VSS", "power_in", g(24)),
        ("3", "SCL", "input", g(16)),
        ("4", "SDA", "bidirectional", g(8)),
        ("5", "GPB0", "bidirectional", g(-4)),
        ("6", "GPB1", "bidirectional", g(-10)),
        ("7", "GPB2", "bidirectional", g(-14)),
        ("8", "GPB3", "bidirectional", g(-18)),
        ("9", "GPB4", "bidirectional", g(-22)),
        ("10", "GPB5", "bidirectional", g(-26)),
        ("11", "GPB6", "bidirectional", g(-30)),
        ("12", "GPB7", "bidirectional", g(-34)),
    ]
    # Right side GPA0..GPA7 spaced by g(9) = 11.43 mm -> zero crowding for resistors/LEDs/switches!
    right_pins = [
        ("13", "GPA0", "bidirectional", g(32)),
        ("14", "GPA1", "bidirectional", g(23)),
        ("15", "GPA2", "bidirectional", g(14)),
        ("16", "GPA3", "bidirectional", g(5)),
        ("17", "GPA4", "bidirectional", g(-5)),
        ("18", "GPA5", "bidirectional", g(-14)),
        ("19", "GPA6", "bidirectional", g(-23)),
        ("20", "GPA7", "bidirectional", g(-32)),
    ]
    pins: List[PinDef] = []
    for num, name, etype, py in left_pins:
        pins.append(PinDef(num, name, etype, lx, py, 0, length=pin_len))
    for num, name, etype, py in right_pins:
        pins.append(PinDef(num, name, etype, rx, py, 180, length=pin_len))

    return LibSymDef(
        lib_id="quad_ipkvm:MCP23017_SSCI016506",
        ref_prefix="U",
        value="MCP23017_SSCI-016506",
        footprint="Module:SSCI-016506_MCP23017",
        pin_names_offset=1.016,
        ref_pos=(0.0, g(38), 0),
        val_pos=(0.0, g(-38), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-half_w)} {fmt(box_top)}) (end {fmt(half_w)} {fmt(box_bot)})"
                " (stroke (width 0.254) (type default)) (fill (type background)))",
            ]
        },
        pins=pins,
    )


def make_74hc139() -> LibSymDef:
    """Dual 2-to-4 line decoder/demultiplexer 74HC139 (DIP-16/SOIC-16, 3 units)."""
    hw = g(6)
    pin_len = g(3)
    lx = round(-hw - pin_len, 2)
    rx = round(hw + pin_len, 2)
    box_top = g(8)
    box_bot = g(-8)

    u1_pins = [
        PinDef("2", "1A", "input", lx, g(5), 0, length=pin_len, unit=1),
        PinDef("3", "1B", "input", lx, g(1), 0, length=pin_len, unit=1),
        PinDef("1", "~{1G}", "input", lx, g(-5), 0, length=pin_len, unit=1),
        PinDef("4", "~{1Y0}", "output", rx, g(6), 180, length=pin_len, unit=1),
        PinDef("5", "~{1Y1}", "output", rx, g(2), 180, length=pin_len, unit=1),
        PinDef("6", "~{1Y2}", "output", rx, g(-2), 180, length=pin_len, unit=1),
        PinDef("7", "~{1Y3}", "output", rx, g(-6), 180, length=pin_len, unit=1),
    ]
    u2_pins = [
        PinDef("14", "2A", "input", lx, g(5), 0, length=pin_len, unit=2),
        PinDef("13", "2B", "input", lx, g(1), 0, length=pin_len, unit=2),
        PinDef("15", "~{2G}", "input", lx, g(-5), 0, length=pin_len, unit=2),
        PinDef("12", "~{2Y0}", "output", rx, g(6), 180, length=pin_len, unit=2),
        PinDef("11", "~{2Y1}", "output", rx, g(2), 180, length=pin_len, unit=2),
        PinDef("10", "~{2Y2}", "output", rx, g(-2), 180, length=pin_len, unit=2),
        PinDef("9", "~{2Y3}", "output", rx, g(-6), 180, length=pin_len, unit=2),
    ]
    u3_pins = [
        PinDef("16", "VCC", "power_in", 0.0, g(5), 270, length=g(2), unit=3),
        PinDef("8", "GND", "power_in", 0.0, g(-5), 90, length=g(2), unit=3),
    ]
    rect = (
        f"(rectangle (start {fmt(-hw)} {fmt(box_top)}) (end {fmt(hw)} {fmt(box_bot)})"
        " (stroke (width 0.254) (type default)) (fill (type background)))"
    )
    pwr_rect = (
        f"(rectangle (start {fmt(g(-3))} {fmt(g(3))}) (end {fmt(g(3))} {fmt(g(-3))})"
        " (stroke (width 0.254) (type default)) (fill (type background)))"
    )
    return LibSymDef(
        lib_id="74xx:74HC139",
        ref_prefix="U",
        value="74HC139",
        footprint="Package_DIP:DIP-16_W7.62mm",
        unit_count=3,
        pin_names_offset=0.762,
        ref_pos=(0.0, g(10), 0),
        val_pos=(0.0, g(-10), 0),
        graphics_by_unit={1: [rect], 2: [rect], 3: [pwr_rect]},
        pins=u1_pins + u2_pins + u3_pins,
    )


def make_photorelay() -> LibSymDef:
    """4-pin PhotoMOS / Photorelay (TLP222A / TLP241A candidate — TODO(hw-verify) C1)."""
    hw = g(4)
    hh = g(3)
    pin_len = g(2)
    lx = round(-hw - pin_len, 2)
    rx = round(hw + pin_len, 2)
    return LibSymDef(
        lib_id="quad_ipkvm:Photorelay_SPST",
        ref_prefix="K",
        value="PhotoMOS_TBD",
        footprint="Package_DIP:DIP-4_W7.62mm",
        pin_names_offset=0.762,
        ref_pos=(0.0, g(5), 0),
        val_pos=(0.0, g(-5), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-hw)} {fmt(hh)}) (end {fmt(hw)} {fmt(-hh)})"
                " (stroke (width 0.254) (type default)) (fill (type background)))",
                "(polyline (pts (xy -2.54 1.524) (xy -1.016 1.524) (xy -1.778 0.254) (xy -2.54 1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
                "(polyline (pts (xy 1.778 1.524) (xy 2.54 0) (xy 1.778 -1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("1", "A", "passive", lx, g(2), 0, length=pin_len),
            PinDef("2", "K", "passive", lx, g(-2), 0, length=pin_len),
            PinDef("3", "S1", "passive", rx, g(2), 180, length=pin_len),
            PinDef("4", "S2", "passive", rx, g(-2), 180, length=pin_len),
        ],
    )


def make_optocoupler() -> LibSymDef:
    """4-pin DC Optocoupler (PC817 / LTV-817 candidate — TODO(hw-verify) C2)."""
    hw = g(4)
    hh = g(3)
    pin_len = g(2)
    lx = round(-hw - pin_len, 2)
    rx = round(hw + pin_len, 2)
    return LibSymDef(
        lib_id="quad_ipkvm:Optocoupler_NPN",
        ref_prefix="OK",
        value="Opto_TBD",
        footprint="Package_DIP:DIP-4_W7.62mm",
        pin_names_offset=0.762,
        ref_pos=(0.0, g(5), 0),
        val_pos=(0.0, g(-5), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-hw)} {fmt(hh)}) (end {fmt(hw)} {fmt(-hh)})"
                " (stroke (width 0.254) (type default)) (fill (type background)))",
                "(polyline (pts (xy -2.54 1.524) (xy -1.016 1.524) (xy -1.778 0.254) (xy -2.54 1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
                "(polyline (pts (xy 1.778 1.524) (xy 1.778 -1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("1", "A", "passive", lx, g(2), 0, length=pin_len),
            PinDef("2", "K", "passive", lx, g(-2), 0, length=pin_len),
            PinDef("4", "C", "open_collector", rx, g(2), 180, length=pin_len),
            PinDef("3", "E", "open_emitter", rx, g(-2), 180, length=pin_len),
        ],
    )


# ---------------------------------------------------------------------------
# Sheet Builder
# ---------------------------------------------------------------------------

class SheetBuilder:
    def __init__(self, key: str, title: str, rev: str, page_num: int, hier_path: str, comment_lines: List[str]):
        self.key = key
        self.uuid = det_uuid(f"sheet:{key}")
        self.title = title
        self.rev = rev
        self.page_num = page_num
        self.hier_path = hier_path
        self.comment_lines = comment_lines
        self.lib_defs: Dict[str, LibSymDef] = {}
        self.items: List[str] = []
        self.pwr_idx = 0
        self.flg_idx = 0

    def ensure_lib(self, lib_def: LibSymDef) -> LibSymDef:
        if lib_def.lib_id not in self.lib_defs:
            self.lib_defs[lib_def.lib_id] = lib_def
        return self.lib_defs[lib_def.lib_id]

    def place_symbol(
        self,
        lib_def: LibSymDef,
        ref: str,
        value: str,
        sx: float,
        sy: float,
        angle: int = 0,
        unit: int = 1,
        footprint: Optional[str] = None,
        ref_offset: Optional[Tuple[float, float, int]] = None,
        val_offset: Optional[Tuple[float, float, int]] = None,
    ) -> Dict[str, Tuple[float, float]]:
        self.ensure_lib(lib_def)
        sx, sy = round(sx, 2), round(sy, 2)
        sym_uuid = det_uuid(f"{self.key}:sym:{ref}:u{unit}:{sx}:{sy}")
        fp = footprint if footprint is not None else lib_def.footprint

        if ref_offset is None:
            rx, ry = rotate_ccw(lib_def.ref_pos[0], lib_def.ref_pos[1], angle)
            ref_cx, ref_cy, ref_ang = round(sx + rx, 2), round(sy - ry, 2), lib_def.ref_pos[2]
        else:
            ref_cx, ref_cy, ref_ang = round(sx + ref_offset[0], 2), round(sy + ref_offset[1], 2), ref_offset[2]

        if val_offset is None:
            vx, vy = rotate_ccw(lib_def.val_pos[0], lib_def.val_pos[1], angle)
            val_cx, val_cy, val_ang = round(sx + vx, 2), round(sy - vy, 2), lib_def.val_pos[2]
        else:
            val_cx, val_cy, val_ang = round(sx + val_offset[0], 2), round(sy + val_offset[1], 2), val_offset[2]

        in_bom = "yes" if lib_def.in_bom else "no"
        on_board = "yes" if lib_def.on_board else "no"
        ref_hide = " hide" if lib_def.is_power else ""

        lines = [
            f'  (symbol (lib_id {q(lib_def.lib_id)}) (at {fmt(sx)} {fmt(sy)} {angle}) (unit {unit})',
            f'    (exclude_from_sim no) (in_bom {in_bom}) (on_board {on_board}) (dnp no)',
            f'    (uuid {q(sym_uuid)})',
            f'    (property "Reference" {q(ref)} (at {fmt(ref_cx)} {fmt(ref_cy)} {ref_ang})'
            f' (effects (font (size 1.27 1.27)){ref_hide}))',
            f'    (property "Value" {q(value)} (at {fmt(val_cx)} {fmt(val_cy)} {val_ang})'
            f' (effects (font (size 1.27 1.27))))',
            f'    (property "Footprint" {q(fp)} (at {fmt(sx)} {fmt(sy)} 0)'
            f' (effects (font (size 1.27 1.27)) hide))',
            f'    (property "Datasheet" "~" (at {fmt(sx)} {fmt(sy)} 0)'
            f' (effects (font (size 1.27 1.27)) hide))',
        ]

        pin_map: Dict[str, Tuple[float, float]] = {}
        for p in lib_def.pins:
            if p.unit != unit:
                continue
            pu = det_uuid(f"{sym_uuid}:pin:{p.number}")
            lines.append(f'    (pin {q(p.number)} (uuid {q(pu)}))')
            pin_map[p.number] = pin_canvas_pos(sx, sy, angle, p.x, p.y)

        lines.append("    (instances")
        lines.append(f'      (project {q(PROJECT_NAME)}')
        lines.append(f'        (path {q(self.hier_path)}')
        lines.append(f'          (reference {q(ref)}) (unit {unit})')
        lines.append("        )")
        lines.append("      )")
        lines.append("    )")
        lines.append("  )")
        self.items.append("\n".join(lines))
        return pin_map

    def place_power(self, net: str, x: float, y: float, angle: int = 0) -> Tuple[float, float]:
        if net == "+3V3":
            lib_def = make_power_3v3()
            self.pwr_idx += 1
            ref = f"#PWR{self.pwr_idx:02d}"
        elif net == "GND":
            lib_def = make_power_gnd()
            self.pwr_idx += 1
            ref = f"#PWR{self.pwr_idx:02d}"
        elif net == "PWR_FLAG":
            lib_def = make_power_flag()
            self.flg_idx += 1
            ref = f"#FLG{self.flg_idx:02d}"
        else:
            raise ValueError(net)
        return self.place_symbol(lib_def, ref, lib_def.value, x, y, angle=angle)["1"]

    def add_wire(self, x1: float, y1: float, x2: float, y2: float) -> None:
        x1, y1, x2, y2 = round(x1, 2), round(y1, 2), round(x2, 2), round(y2, 2)
        if (x1, y1) == (x2, y2):
            return
        u = det_uuid(f"{self.key}:wire:{len(self.items)}:{x1}:{y1}:{x2}:{y2}")
        self.items.append(
            f'  (wire (pts (xy {fmt(x1)} {fmt(y1)}) (xy {fmt(x2)} {fmt(y2)}))\n'
            f'    (stroke (width 0) (type default))\n'
            f'    (uuid {q(u)})\n'
            f'  )'
        )

    def add_junction(self, x: float, y: float) -> None:
        x, y = round(x, 2), round(y, 2)
        u = det_uuid(f"{self.key}:junc:{x}:{y}")
        self.items.append(
            f'  (junction (at {fmt(x)} {fmt(y)}) (diameter 0) (color 0 0 0 0)\n'
            f'    (uuid {q(u)})\n'
            f'  )'
        )

    def add_nc(self, x: float, y: float) -> None:
        x, y = round(x, 2), round(y, 2)
        u = det_uuid(f"{self.key}:nc:{x}:{y}")
        self.items.append(f'  (no_connect (at {fmt(x)} {fmt(y)}) (uuid {q(u)}))')

    def add_label(self, name: str, x: float, y: float, angle: int = 0) -> None:
        """
        Place net label at (x, y).
        Convention: angle=0 places text RIGHT of (x, y); angle=180 places text LEFT of (x, y).
        Wait: on wire right endpoint use angle=0 (text extends right, away from wire);
        on wire left endpoint use angle=180 (text extends left, away from wire).
        """
        x, y = round(x, 2), round(y, 2)
        u = det_uuid(f"{self.key}:lbl:{len(self.items)}:{name}:{x}:{y}:{angle}")
        just = "right bottom" if angle == 180 else "left bottom"
        self.items.append(
            f'  (label {q(name)} (at {fmt(x)} {fmt(y)} {angle}) (fields_autoplaced yes)\n'
            f'    (effects (font (size 1.27 1.27)) (justify {just}))\n'
            f'    (uuid {q(u)})\n'
            f'  )'
        )

    def add_hlabel(self, name: str, shape: str, x: float, y: float, angle: int = 180) -> None:
        """angle=180 -> flag extends LEFT of (x,y); angle=0 -> flag extends RIGHT of (x,y)."""
        x, y = round(x, 2), round(y, 2)
        u = det_uuid(f"{self.key}:hlbl:{name}:{x}:{y}:{angle}")
        just = "right" if angle == 180 else "left"
        self.items.append(
            f'  (hierarchical_label {q(name)} (shape {shape}) (at {fmt(x)} {fmt(y)} {angle}) (fields_autoplaced yes)\n'
            f'    (effects (font (size 1.27 1.27)) (justify {just}))\n'
            f'    (uuid {q(u)})\n'
            f'  )'
        )

    def add_text(self, text: str, x: float, y: float, size: float = 1.27, bold: bool = False,
                 color: Optional[Tuple[int, int, int, float]] = None, just: str = "left top") -> None:
        x, y = round(x, 2), round(y, 2)
        u = det_uuid(f"{self.key}:txt:{len(self.items)}:{x}:{y}")
        b_str = " bold" if bold else ""
        c_str = f" (color {color[0]} {color[1]} {color[2]} {color[3]})" if color else ""
        self.items.append(
            f'  (text {q(text)} (exclude_from_sim no)\n'
            f'    (at {fmt(x)} {fmt(y)} 0)\n'
            f'    (effects (font (size {fmt(size)} {fmt(size)}){b_str}{c_str}) (justify {just}))\n'
            f'    (uuid {q(u)})\n'
            f'  )'
        )

    def add_rect(self, x1: float, y1: float, x2: float, y2: float, width: float = 0.254,
                 color: Optional[Tuple[int, int, int, float]] = None) -> None:
        u = det_uuid(f"{self.key}:rect:{len(self.items)}:{x1}:{y1}:{x2}:{y2}")
        c_str = f" (color {color[0]} {color[1]} {color[2]} {color[3]})" if color else ""
        self.items.append(
            f'  (rectangle (start {fmt(x1)} {fmt(y1)}) (end {fmt(x2)} {fmt(y2)})\n'
            f'    (stroke (width {fmt(width)}) (type solid){c_str}) (fill (type none))\n'
            f'    (uuid {q(u)})\n'
            f'  )'
        )

    def add_sheet_block(
        self,
        block_uuid: str,
        name: str,
        filename: str,
        x: float,
        y: float,
        w: float,
        h: float,
        pins: List[Tuple[str, str, float, float, int]],
        page_str: str,
    ) -> None:
        lines = [
            f'  (sheet (at {fmt(x)} {fmt(y)}) (size {fmt(w)} {fmt(h)})',
            f'    (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no) (fields_autoplaced yes)',
            f'    (stroke (width 0.254) (type solid)) (fill (color 245 248 255 0.6))',
            f'    (uuid {q(block_uuid)})',
            f'    (property "Sheetname" {q(name)} (at {fmt(x)} {fmt(round(y - 1.27, 2))} 0)',
            f'      (effects (font (size 1.524 1.524) bold) (justify left bottom)))',
            f'    (property "Sheetfile" {q(filename)} (at {fmt(x)} {fmt(round(y + h + 1.27, 2))} 0)',
            f'      (effects (font (size 1.27 1.27)) (justify left top)))',
        ]
        for pname, pshape, px, py, pang in pins:
            pu = det_uuid(f"{self.key}:sheetpin:{block_uuid}:{pname}")
            just = "left" if pang == 180 else "right"
            lines.append(
                f'    (pin {q(pname)} {pshape} (at {fmt(px)} {fmt(py)} {pang})'
                f' (effects (font (size 1.27 1.27)) (justify {just})) (uuid {q(pu)}))'
            )
        lines.append("    (instances")
        lines.append(f'      (project {q(PROJECT_NAME)}')
        lines.append(f'        (path {q(self.hier_path)} (page {q(page_str)}))')
        lines.append("      )")
        lines.append("    )")
        lines.append("  )")
        self.items.append("\n".join(lines))

    def render_sch(self, is_root: bool = False) -> str:
        c_lines = "\n".join(f'    (comment {i + 1} {q(c)})' for i, c in enumerate(self.comment_lines[:4]))
        lib_block = "\n".join(self.lib_defs[k].to_sexpr() for k in sorted(self.lib_defs))
        body = "\n".join(self.items)
        extra_tail = '\n  (sheet_instances\n    (path "/" (page "1"))\n  )' if is_root else ""
        return (
            f'(kicad_sch\n'
            f'  (version {KICAD_VERSION})\n'
            f'  (generator {q(GENERATOR_NAME)})\n'
            f'  (generator_version "1.0")\n'
            f'  (uuid {q(self.uuid)})\n'
            f'  (paper "A3")\n'
            f'  (title_block\n'
            f'    (title {q(self.title)})\n'
            f'    (date "2026-11-27")\n'
            f'    (rev {q(self.rev)})\n'
            f'    (company "quad-ipkvm")\n'
            f'{c_lines}\n'
            f'  )\n'
            f'  (lib_symbols\n'
            f'{lib_block}\n'
            f'  )\n'
            f'{body}{extra_tail}\n'
            f')\n'
        )


# ---------------------------------------------------------------------------
# Sheet Content Builders
# ---------------------------------------------------------------------------

def build_root_sheet(child_uuids: Dict[str, str]) -> SheetBuilder:
    root_uuid = det_uuid("sheet:root")
    sb = SheetBuilder(
        key="root",
        title="quad-ipkvm Top-Level Interconnect (ESP32-P4-ETH rev v1.3)",
        rev="1.0",
        page_num=1,
        hier_path=f"/{root_uuid}",
        comment_lines=[
            "Primary reference: docs/WIRING.md & docs/ESP32-P4-ETH-schematic.pdf",
            "PR #174: VBUS R1=130k/R2=162k 0.5% E96, NO J3 ESD diode",
            "Safety: NEVER connect J3 #1 (VCC_5V) to PC VBUS; NEVER connect X630 #2 HDET (4.8V)",
            "TODO(hw-verify): ATX/KVM GPIOs on P1 are PROVISIONAL until A1-A8/C1-C7 verified",
        ],
    )

    sb.add_text("quad-ipkvm Top-Level Hardware Architecture (Waveshare ESP32-P4-ETH rev v1.3)",
                g(14), g(12), size=2.4, bold=True)

    p1_sym = make_p1_40pin()
    p1_x, p1_y = g(68), g(72)
    p1_pins = sb.place_symbol(p1_sym, "P1", "ESP32-P4-ETH_P1_2x20", p1_x, p1_y, angle=0)

    # P1 left column (1..20)
    for pin_num in ["1", "2", "7", "9", "10"]:
        sb.add_nc(*p1_pins[pin_num])
    sb.add_text("[SPARE SAFE GPIOs]", g(14), p1_pins["1"][1] - g(1), size=1.14, color=(20, 110, 40, 1.0))

    for pin_num in ["11", "12", "14", "15"]:
        sb.add_nc(*p1_pins[pin_num])
    sb.add_text("[JTAG AVOID - kvm_board rejects out]", g(12), p1_pins["11"][1] - g(1), size=1.14, color=(150, 90, 0, 1.0))

    for pin_num in ["19", "20"]:
        sb.add_nc(*p1_pins[pin_num])
    sb.add_text("[PCB FIXED USB Serial/JTAG]", g(14), p1_pins["19"][1] - g(1), size=1.14, color=(30, 80, 160, 1.0))

    # Left GND pins (3, 8, 13, 18) + PWR_FLAG cleanly offset
    for idx, pin_num in enumerate(["3", "8", "13", "18"]):
        px, py = p1_pins[pin_num]
        gx = px - g(6)
        sb.add_wire(px, py, gx, py)
        sb.place_power("GND", gx, py)
        if idx == 0:
            fx = gx - g(10)
            sb.add_wire(gx, py, fx, py)
            sb.add_junction(gx, py)
            sb.place_power("PWR_FLAG", fx, py)

    # Left active signals (wire goes LEFT -> label at LEFT end with angle=180 so text extends LEFT)
    for pin_num, lbl in [("4", "VBUS_SENSE"), ("5", "X630_INT"), ("6", "X630_RESET_N"),
                         ("16", "I2C_SCL"), ("17", "I2C_SDA")]:
        px, py = p1_pins[pin_num]
        lx = px - g(8)
        sb.add_wire(px, py, lx, py)
        sb.add_label(lbl, lx, py, angle=180)

    # P1 right column (40..21)
    for pin_num in ["40", "39", "37", "30", "29", "27"]:
        sb.add_nc(*p1_pins[pin_num])
    sb.add_text("[H2 USB-C ONLY; NO EXT 5V]", p1_pins["40"][0] + g(3), p1_pins["40"][1] - g(1), size=1.14, color=(190, 20, 20, 1.0))
    sb.add_text("[PCB FIXED USB_FS OTG]", p1_pins["29"][0] + g(3), p1_pins["29"][1] - g(1), size=1.14, color=(30, 80, 160, 1.0))

    for pin_num in ["38", "33", "28", "23"]:
        px, py = p1_pins[pin_num]
        gx = px + g(6)
        sb.add_wire(px, py, gx, py)
        sb.place_power("GND", gx, py)

    # Pin 36 (+3V3 + well-separated PWR_FLAG)
    p36x, p36y = p1_pins["36"]
    pwr_x = p36x + g(8)
    flg_x = pwr_x + g(14)
    sb.add_wire(p36x, p36y, pwr_x, p36y)
    sb.add_wire(pwr_x, p36y, flg_x, p36y)
    sb.add_junction(pwr_x, p36y)
    sb.place_power("+3V3", pwr_x, p36y)
    sb.place_power("PWR_FLAG", flg_x, p36y)
    sb.add_text("meas 3.34V [V2]", flg_x + g(2), p36y - g(1), size=1.14)

    # Right active signals (wire goes RIGHT -> label at RIGHT end with angle=0 so text extends RIGHT)
    for pin_num, lbl in [
        ("35", "KVM_A0"), ("34", "KVM_A1"), ("32", "~{E_PWR}"), ("31", "~{E_RST}"),
        ("26", "KVM_LED_CH1"), ("25", "KVM_LED_CH2"), ("24", "~{E_KVM}"),
        ("22", "KVM_LED_CH3"), ("21", "KVM_LED_CH4"),
    ]:
        px, py = p1_pins[pin_num]
        lx = px + g(8)
        sb.add_wire(px, py, lx, py)
        sb.add_label(lbl, lx, py, angle=0)

    # J3 USB OTG HS 4-pin header
    j3_sym = make_conn_1xN(
        "quad_ipkvm:ESP32-P4-ETH_J3",
        4,
        ["VCC_5V (NC!)", "USBD_N", "USBD_P", "GND"],
        ["passive", "bidirectional", "bidirectional", "power_in"],
        width_units=16,
        spacing_units=4,
        footprint="Connector_JST:JST_SH_BM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal",
    )
    j3_pins = sb.place_symbol(j3_sym, "J3", "USB_OTG_HS_4P", g(32), g(148))
    sb.add_nc(*j3_pins["1"])
    sb.add_text("FORBIDDEN: DO NOT CONNECT TO PC VBUS [H3]!", j3_pins["1"][0] + g(2), j3_pins["1"][1] - g(1),
                size=1.27, bold=True, color=(200, 0, 0, 1.0))
    for pnum, lbl in [("2", "USB_DM"), ("3", "USB_DP")]:
        px, py = j3_pins[pnum]
        sb.add_wire(px, py, px + g(8), py)
        sb.add_label(lbl, px + g(8), py, angle=0)
    g4x, g4y = j3_pins["4"]
    sb.add_wire(g4x, g4y, g4x + g(6), g4y)
    sb.place_power("GND", g4x + g(6), g4y)

    # 4 Hierarchical Sheet Blocks (pin wires go LEFT -> label at LEFT end angle=180)
    b1_x, b1_y, b1_w, b1_h = g(148), g(20), g(56), g(28)
    b1_pins = [
        ("USB_DM", "bidirectional", b1_x, b1_y + g(7), 180),
        ("USB_DP", "bidirectional", b1_x, b1_y + g(14), 180),
        ("VBUS_SENSE", "output", b1_x, b1_y + g(21), 180),
    ]
    sb.add_sheet_block(child_uuids["usb_vbus"], "USB_VBUS", "usb_vbus.kicad_sch", b1_x, b1_y, b1_w, b1_h, b1_pins, "2")
    for pname, _, px, py, _ in b1_pins:
        sb.add_wire(px - g(10), py, px, py)
        sb.add_label(pname, px - g(10), py, angle=180)

    b2_x, b2_y, b2_w, b2_h = g(148), g(58), g(56), g(34)
    b2_pins = [
        ("I2C_SDA", "bidirectional", b2_x, b2_y + g(7), 180),
        ("I2C_SCL", "output", b2_x, b2_y + g(14), 180),
        ("X630_RESET_N", "input", b2_x, b2_y + g(21), 180),
        ("X630_INT", "output", b2_x, b2_y + g(28), 180),
    ]
    sb.add_sheet_block(child_uuids["x630_csi"], "X630_CSI", "x630_csi.kicad_sch", b2_x, b2_y, b2_w, b2_h, b2_pins, "3")
    for pname, _, px, py, _ in b2_pins:
        sb.add_wire(px - g(10), py, px, py)
        sb.add_label(pname, px - g(10), py, angle=180)

    b3_x, b3_y, b3_w, b3_h = g(148), g(102), g(56), g(22)
    b3_pins = [
        ("I2C_SDA", "bidirectional", b3_x, b3_y + g(7), 180),
        ("I2C_SCL", "input", b3_x, b3_y + g(14), 180),
    ]
    sb.add_sheet_block(child_uuids["i2c_oled_panel"], "I2C_OLED_PANEL", "i2c_oled_panel.kicad_sch", b3_x, b3_y, b3_w, b3_h, b3_pins, "4")
    for pname, _, px, py, _ in b3_pins:
        sb.add_wire(px - g(10), py, px, py)
        sb.add_label(pname, px - g(10), py, angle=180)

    b4_x, b4_y, b4_w, b4_h = g(148), g(134), g(62), g(68)
    b4_pins = [
        ("KVM_A0", "input", b4_x, b4_y + g(6), 180),
        ("KVM_A1", "input", b4_x, b4_y + g(13), 180),
        ("~{E_PWR}", "input", b4_x, b4_y + g(20), 180),
        ("~{E_RST}", "input", b4_x, b4_y + g(27), 180),
        ("~{E_KVM}", "input", b4_x, b4_y + g(34), 180),
        ("KVM_LED_CH1", "output", b4_x, b4_y + g(42), 180),
        ("KVM_LED_CH2", "output", b4_x, b4_y + g(49), 180),
        ("KVM_LED_CH3", "output", b4_x, b4_y + g(56), 180),
        ("KVM_LED_CH4", "output", b4_x, b4_y + g(63), 180),
    ]
    sb.add_sheet_block(child_uuids["atx_kvm_ctrl"], "ATX_KVM_CTRL", "atx_kvm_ctrl.kicad_sch", b4_x, b4_y, b4_w, b4_h, b4_pins, "5")
    for pname, _, px, py, _ in b4_pins:
        sb.add_wire(px - g(10), py, px, py)
        sb.add_label(pname, px - g(10), py, angle=180)

    # Bottom-Left Safety Rules Box
    sb.add_rect(g(14), g(172), g(120), g(222), width=0.254, color=(180, 0, 0, 1.0))
    sb.add_text(
        "CRITICAL SAFETY RULES & OWNER DECISIONS (docs/WIRING.md)\n"
        "1. [H3] NEVER connect J3 #1 (VCC_5V) to PC USB VBUS. Only D-/D+/GND go to J3.\n"
        "2. [HW_STATE] NEVER connect X630 10-pin #2 (HDET = 4.8V) to ESP32-P4 GPIO (3.3V max).\n"
        "3. [PLAN 4.4] ATX/KVM outputs & KVM LED inputs MUST use direct GPIO (NOT MCP23017).\n"
        "4. [PR #174] VBUS divider: R1=130k, R2=162k (0.5% E96); NO ESD diode on J3 (#4/#174).\n"
        "5. [PWR SEQ] Power ESP32-P4 via H2 Type-C BEFORE connecting target PC VBUS.\n"
        "6. [PROVISIONAL] P1 GPIOs 20..23,46 (ATX/KVM out) & 32,33,47,48 (KVM LED in) are\n"
        "   PROVISIONAL TODO(hw-verify) candidates from safe pool; finalize via Kconfig!",
        g(16), g(175), size=1.35, color=(120, 10, 10, 1.0)
    )

    # Right Side Info Boxes (kept inside x <= 406 mm, y <= 185 mm)
    sb.add_rect(g(220), g(20), g(318), g(116), width=0.254, color=(30, 80, 160, 1.0))
    sb.add_text(
        "PCB-FIXED INTERFACES ON ESP32-P4-ETH\n"
        "(Internal to Waveshare PCB — No external P1 wiring)\n\n"
        "- H2 Type-C UART: U4 CH343P -> GPIO37/38, +5V via AO3401/D2\n"
        "- J4 100M Ethernet: U10 IP101GRI -> GPIO28..31,34,35,49..52\n"
        "- TF1 microSD 4-bit: GPIO39..44, PMOS Q1 gate GPIO45 (+10k PD)\n"
        "    Pull-ups R8..R12 (51k) -> ESP_LDO_VO4 (3300 mV)\n"
        "- J1 MIPI-CSI 22-pin: CSI_CLK/D0/D1, R44/R46 2.2k I2C PU\n"
        "    (Detailed on Sheet 3: x630_csi.kicad_sch)\n"
        "- U9/U11 Audio (unused): I2S GPIO9..13, GPIO53 PA_CTRL,\n"
        "    I2C shared on GPIO7/8 (check ES8311 addr conflict G2)\n"
        "- Boot/Reset: Key1 BOOT=GPIO35, Key2 RESET=CHIP_PU (3.3V)\n"
        "- Strapping: GPIO34..38 (no external drive allowed)\n"
        "- Power Rails: MP1658GTF-Z ESP_3V3 (meas 3.34V),\n"
        "    LDO_VO3=2500mV (MIPI PHY), LDO_VO4=3300mV (SD PU)",
        g(223), g(23), size=1.3, color=(20, 50, 120, 1.0)
    )

    sb.add_rect(g(220), g(124), g(318), g(180), width=0.254, color=(80, 40, 140, 1.0))
    sb.add_text(
        "SHEET DIRECTORY & GPIO BUDGET\n\n"
        "Page 1 (Root): P1 40-pin + J3 USB OTG + System Interconnect\n"
        "Page 2 (USB_VBUS): Target PC USB + R1/R2 0.5% E96 VBUS Sense\n"
        "Page 3 (X630_CSI): J1 22p <-> X630 15p CSI + X630 10p Ctrl\n"
        "Page 4 (I2C_OLED_PANEL): SSD1306 (0x3C/3D) + MCP23017 (0x20)\n"
        "Page 5 (ATX_KVM_CTRL): 74HC139 x2 + 12x Photorelay + 4x Opto\n\n"
        "Remaining P1 Safe Spare GPIOs (5 pins after provisional):\n"
        "  GPIO6, GPIO14, GPIO15, GPIO19, GPIO54 (+ JTAG GPIO2..5)",
        g(223), g(127), size=1.3, color=(60, 30, 110, 1.0)
    )

    return sb


def build_usb_vbus_sheet(root_uuid: str, block_uuid: str) -> SheetBuilder:
    sb = SheetBuilder(
        key="usb_vbus",
        title="Target PC USB OTG HS (J3) & VBUS Sense Divider",
        rev="1.0",
        page_num=2,
        hier_path=f"/{root_uuid}/{block_uuid}",
        comment_lines=[
            "docs/WIRING.md §6.2 & docs/USB_VBUS_DESIGN.md §3.1 (USB 2.0 §7.2.1)",
            "Owner decision (#174): 0.5% E96 R1=130k, R2=162k; NO ESD diode on J3 (H4)",
        ],
    )
    sb.add_text("Sheet 2: Target PC USB OTG HS Data (3-wire) & VBUS Sense Divider", g(14), g(12), size=2.2, bold=True)

    j2_sym = make_conn_1xN(
        "quad_ipkvm:Target_PC_USB_4P",
        4,
        ["VBUS_PC (4.75-5.50V)", "D-", "D+", "GND"],
        ["passive", "bidirectional", "bidirectional", "power_in"],
        width_units=20,
        spacing_units=6,
        footprint="Connector_USB:USB_A_Molex_67643_Horizontal",
    )
    j2_pins = sb.place_symbol(j2_sym, "J2", "Target_PC_USB_4P", g(46), g(70))

    # D- and D+ straight to hierarchical labels (well below VBUS divider at y-g(14))
    for pnum, hname in [("2", "USB_DM"), ("3", "USB_DP")]:
        px, py = j2_pins[pnum]
        hx = px + g(20)
        sb.add_wire(px, py, hx, py)
        sb.add_hlabel(hname, "bidirectional", hx, py, angle=0)

    # GND on pin 4
    gpx, gpy = j2_pins["4"]
    sb.add_wire(gpx, gpy, gpx + g(8), gpy)
    sb.place_power("GND", gpx + g(8), gpy)

    # Route J2 pin 1 (VBUS_PC) UP to y = v1y - g(18) so divider is completely clear of D-/D+!
    r_sym = make_resistor()
    v1x, v1y = j2_pins["1"]
    top_y = v1y - g(18)
    sb.add_wire(v1x, v1y, v1x, top_y)
    r1_cx = v1x + g(20)
    r1_pins = sb.place_symbol(r_sym, "R1", "130k_0.5%_E96", r1_cx, top_y, angle=90,
                              ref_offset=(0, -g(3), 0), val_offset=(0, g(3), 0))
    sb.add_wire(v1x, top_y, r1_pins["1"][0], r1_pins["1"][1])

    mid_x = r1_pins["2"][0] + g(14)
    sb.add_wire(r1_pins["2"][0], r1_pins["2"][1], mid_x, top_y)
    sb.add_junction(mid_x, top_y)

    # VBUS_SENSE goes RIGHT
    vbus_hx = mid_x + g(20)
    sb.add_wire(mid_x, top_y, vbus_hx, top_y)
    sb.add_hlabel("VBUS_SENSE", "output", vbus_hx, top_y, angle=0)

    # R2 hangs DOWN to GND (x = mid_x >= g(100), completely right of USB_DM/DP at g(80)!)
    r2_cy = top_y + g(12)
    r2_pins = sb.place_symbol(r_sym, "R2", "162k_0.5%_E96", mid_x, r2_cy, angle=0,
                              ref_offset=(g(4), -g(1), 0), val_offset=(g(4), g(1), 0))
    sb.add_wire(mid_x, top_y, r2_pins["1"][0], r2_pins["1"][1])
    sb.add_wire(r2_pins["2"][0], r2_pins["2"][1], r2_pins["2"][0], r2_pins["2"][1] + g(5))
    sb.place_power("GND", r2_pins["2"][0], r2_pins["2"][1] + g(5))

    sb.add_rect(g(14), g(104), g(195), g(168), width=0.254, color=(20, 90, 40, 1.0))
    sb.add_text(
        "VBUS DIVIDER DESIGN & VERIFICATION NOTES (docs/WIRING.md §6.2.1)\n"
        "- Owner decision (2026-11-26, #174): Use 0.5% E96 pair R1=130 kOhm, R2=162 kOhm.\n"
        "  (Alternative 1% E24 pair: R1=120k, R2=150k — NEVER mix E96 and E24 pairs!)\n"
        "- Ratio R2/(R1+R2) = 0.5548: VBUS=4.75V -> 2.635V (> VIH_max 2.53V);\n"
        "  VBUS=5.50V -> 3.051V (< VDD_min 3.18V). ESP_3V3 measured 3.34V [V2].\n"
        "- Max power ~0.11 mW; standard THT 1/4W metal-film +/-0.5% resistors suffice.\n"
        "- No ESD clamp diode added on J3 or VBUS sense (#174 / H4 decision).\n"
        "  R1 (130 kOhm) acts as series current limiter on VBUS sense.\n"
        "- Kconfig: CONFIG_USB_DEVICE_VBUS_SENSE_GPIO=18, BVALID_OVERRIDE=y, POLL=100ms.\n"
        "- Safety: PC VBUS MUST NOT touch J3 #1 (VCC_5V). Power ESP32-P4 via H2 BEFORE PC!",
        g(17), g(107), size=1.35, color=(15, 70, 30, 1.0)
    )

    return sb


def build_x630_csi_sheet(root_uuid: str, block_uuid: str) -> SheetBuilder:
    sb = SheetBuilder(
        key="x630_csi",
        title="Geekworm X630 V1.5 (TC358743XBG) MIPI-CSI & Control",
        rev="1.0",
        page_num=3,
        hier_path=f"/{root_uuid}/{block_uuid}",
        comment_lines=[
            "docs/WIRING.md §6.1(a) 15pin-to-22pin FFC & §6.1(b) 10pin SH1.0 header",
            "SAFETY: X630 10-pin #2 HDET outputs 4.8V -> FORBIDDEN to connect to ESP32-P4!",
        ],
    )
    sb.add_text("Sheet 3: Geekworm X630 V1.5 (TC358743XBG) — CSI FFC & 10-pin Control", g(14), g(12), size=2.2, bold=True)

    j1_names = [
        "1:3V3", "2:SDA(G7)", "3:SCL(G8)", "4:GND?", "5:CSI_IO1",
        "6:CSI_IO0", "7:GND", "8:NC", "9:NC", "10:NC",
        "11:NC", "12:NC", "13:GND", "14:CSI_CLKP", "15:CSI_CLKN",
        "16:GND", "17:CSI_D1P", "18:CSI_D1N", "19:GND", "20:CSI_D0P",
        "21:CSI_D0N", "22:GND"
    ]
    j1_types = [
        "power_in", "bidirectional", "input", "passive", "bidirectional",
        "bidirectional", "passive", "passive", "passive", "passive",
        "passive", "passive", "power_in", "bidirectional", "bidirectional",
        "power_in", "bidirectional", "bidirectional", "power_in", "bidirectional",
        "bidirectional", "power_in"
    ]
    j1_sym = make_conn_1xN(
        "quad_ipkvm:ESP32-P4-ETH_J1_CSI_22P", 22, j1_names, j1_types,
        width_units=16, spacing_units=4,
        footprint="Connector_FFC-FPC:Hirose_FH12-22S-0.5SH_1x22-1MP_P0.50mm_Horizontal"
    )
    j1_pins = sb.place_symbol(j1_sym, "J1", "ESP_J1_CSI_22P", g(32), g(80))

    j4_names = [
        "1:3V3", "2:SDA", "3:SCL", "4:REFCLK?[est]", "5:IO0?[est]",
        "6:GND", "7:CK_P", "8:CK_N", "9:GND", "10:D1_P",
        "11:D1_N", "12:GND", "13:D0_P", "14:D0_N", "15:GND"
    ]
    j4_sym = make_conn_1xN_left(
        "quad_ipkvm:X630_CSI_15P", 15, j4_names,
        width_units=16, spacing_units=4,
        footprint="Connector_FFC-FPC:Molex_52207-1585_1x15-1MP_P1.00mm_Horizontal"
    )
    j4_pins = sb.place_symbol(j4_sym, "J4", "X630_CSI_15P", g(138), g(66))

    for p in ["4", "7", "8", "9", "10", "11", "12"]:
        sb.add_nc(*j1_pins[p])

    # Single shared GND bus trunk for J1 (13,16,19,22) and J4 (6,9,12,15) -> ZERO overlap with adjacent signal labels!
    j1_gnds = [j1_pins[p] for p in ["13", "16", "19", "22"]]
    j1_gx = j1_gnds[0][0] + g(14)
    for idx, (px, py) in enumerate(j1_gnds):
        sb.add_wire(px, py, j1_gx, py)
        if idx > 0:
            sb.add_wire(j1_gx, j1_gnds[idx - 1][1], j1_gx, py)
            sb.add_junction(j1_gx, py)
    sb.add_wire(j1_gx, j1_gnds[-1][1], j1_gx, j1_gnds[-1][1] + g(4))
    sb.place_power("GND", j1_gx, j1_gnds[-1][1] + g(4))

    j4_gnds = [j4_pins[p] for p in ["6", "9", "12", "15"]]
    j4_gx = j4_gnds[0][0] - g(14)
    for idx, (px, py) in enumerate(j4_gnds):
        sb.add_wire(px, py, j4_gx, py)
        if idx > 0:
            sb.add_wire(j4_gx, j4_gnds[idx - 1][1], j4_gx, py)
            sb.add_junction(j4_gx, py)
    sb.add_wire(j4_gx, j4_gnds[-1][1], j4_gx, j4_gnds[-1][1] + g(4))
    sb.place_power("GND", j4_gx, j4_gnds[-1][1] + g(4))

    ffc_nets = [
        ("1", "1", "FFC_3V3"),
        ("2", "2", "I2C_SDA"),
        ("3", "3", "I2C_SCL"),
        ("5", "4", "FFC_IO1_R43NC"),
        ("6", "5", "FFC_IO0_R42PU"),
        ("14", "7", "CSI_CLK_P"),
        ("15", "8", "CSI_CLK_N"),
        ("17", "10", "CSI_D1_P"),
        ("18", "11", "CSI_D1_N"),
        ("20", "13", "CSI_D0_P"),
        ("21", "14", "CSI_D0_N"),
    ]
    for p1, p4, nname in ffc_nets:
        x1, y1 = j1_pins[p1]
        x4, y4 = j4_pins[p4]
        if nname == "FFC_3V3":
            sb.add_wire(x1, y1, x1 + g(6), y1)
            sb.place_power("+3V3", x1 + g(6), y1)
            sb.add_wire(x4, y4, x4 - g(6), y4)
            sb.place_power("+3V3", x4 - g(6), y4)
        else:
            # Rightward wire from J1 -> angle=0 puts text to the RIGHT cleanly!
            sb.add_wire(x1, y1, x1 + g(8), y1)
            sb.add_label(nname, x1 + g(8), y1, angle=0)
            # Leftward wire from J4 -> angle=180 puts text to the LEFT cleanly!
            sb.add_wire(x4, y4, x4 - g(8), y4)
            sb.add_label(nname, x4 - g(8), y4, angle=180)

    # Hierarchical I2C labels
    for idx, (hname, shape) in enumerate([("I2C_SDA", "bidirectional"), ("I2C_SCL", "output")]):
        hx, hy = g(196), g(34 + idx * 8)
        sb.add_hlabel(hname, shape, hx, hy, angle=180)
        sb.add_wire(hx, hy, hx + g(8), hy)
        sb.add_label(hname, hx + g(8), hy, angle=0)

    # J5: X630 10-pin SH1.0 Connector
    j5_names = [
        "1:3V3_OUT", "2:HDET_4.8V!", "3:IR", "4:INT", "5:RESET_N",
        "6:A_SCK", "7:A_WFS", "8:A_OSCK", "9:A_SD", "10:GND"
    ]
    j5_sym = make_conn_1xN(
        "quad_ipkvm:X630_10P_SH1", 10, j5_names,
        width_units=16, spacing_units=6,
        footprint="Connector_JST:JST_SH_BM10B-SRSS-TB_1x10-1MP_P1.00mm_Horizontal"
    )
    j5_pins = sb.place_symbol(j5_sym, "J5", "X630_10P_SH1.0", g(196), g(96))
    for p in ["1", "2", "3", "6", "7", "8", "9"]:
        sb.add_nc(*j5_pins[p])
    sb.add_text("FORBIDDEN: 4.8V! DO NOT CONNECT", j5_pins["2"][0] + g(2), j5_pins["2"][1] - g(1),
                size=1.27, bold=True, color=(200, 0, 0, 1.0))

    x4, y4 = j5_pins["4"]
    sb.add_wire(x4, y4, x4 + g(14), y4)
    sb.add_hlabel("X630_INT", "output", x4 + g(14), y4, angle=0)
    sb.add_text("TODO(hw-verify) B6: Check High V", x4 + g(2), y4 - g(2), size=1.14, color=(120, 30, 150, 1.0))

    x5, y5 = j5_pins["5"]
    sb.add_wire(x5, y5, x5 + g(14), y5)
    sb.add_hlabel("X630_RESET_N", "input", x5 + g(14), y5, angle=0)
    sb.add_text("OD Active-Low, ~20k PU [Verified]", x5 + g(2), y5 + g(2), size=1.14, color=(20, 110, 40, 1.0))

    gx, gy = j5_pins["10"]
    sb.add_wire(gx, gy, gx + g(6), gy)
    sb.place_power("GND", gx + g(6), gy)

    sb.add_rect(g(14), g(158), g(240), g(196), width=0.254, color=(30, 80, 160, 1.0))
    sb.add_text(
        "X630 V1.5 (TC358743XBG) NOTES (docs/WIRING.md §6.1)\n"
        "- I2C Address: 0x0F (verified at 100 kHz, 27 MHz onboard XTAL, Chip ID 0x0000).\n"
        "- CSI 2-lane capture verified (1080p30 / 720p60) via 15p <-> 22p conversion FFC.\n"
        "- J1 R44/R46 2.2k I2C pull-ups to ESP_3V3 on ESP32-P4-ETH PCB.\n"
        "- X630 #5 RESET wired to P1 #6 (GPIO16, Open-Drain); #10 GND wired to P1 #8.",
        g(17), g(161), size=1.35, color=(20, 50, 120, 1.0)
    )

    return sb


def build_i2c_oled_sheet(root_uuid: str, block_uuid: str) -> SheetBuilder:
    sb = SheetBuilder(
        key="i2c_oled_panel",
        title="Shared I2C Bus, SSD1306 OLED & MCP23017 Panel",
        rev="1.0",
        page_num=4,
        hier_path=f"/{root_uuid}/{block_uuid}",
        comment_lines=[
            "docs/WIRING.md §6.3 & docs/OLED3_PLAN.md (I2C 3.3V <=400kHz, PCB 2.2k PU R44/R46)",
            "CRITICAL: SSCI-016506 /RESET tied high internally — NEVER place ATX/KVM on MCP23017!",
        ],
    )
    sb.add_text("Sheet 4: Shared I2C Bus — SSD1306 OLED (0x3C/3D) & MCP23017 Panel (0x20)", g(14), g(12), size=2.2, bold=True)

    # Hierarchical I2C Inputs
    sb.add_hlabel("I2C_SDA", "bidirectional", g(32), g(28), angle=180)
    sb.add_wire(g(32), g(28), g(42), g(28))
    sb.add_label("I2C_SDA", g(42), g(28), angle=0)

    sb.add_hlabel("I2C_SCL", "input", g(32), g(36), angle=180)
    sb.add_wire(g(32), g(36), g(42), g(36))
    sb.add_label("I2C_SCL", g(42), g(36), angle=0)

    # J6: SSD1306 OLED with g(8) = 10.16 mm spacing so +3V3 & GND have zero text collision!
    j6_sym = make_conn_1xN(
        "quad_ipkvm:SSD1306_OLED_4P", 4,
        ["GND", "VCC(+3V3)", "SCL", "SDA"],
        ["power_in", "power_in", "input", "bidirectional"],
        width_units=16, spacing_units=8,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical"
    )
    j6_pins = sb.place_symbol(j6_sym, "J6", "SSD1306_OLED_0x3C", g(40), g(68))
    sb.add_wire(j6_pins["1"][0], j6_pins["1"][1], j6_pins["1"][0] + g(8), j6_pins["1"][1])
    sb.place_power("GND", j6_pins["1"][0] + g(8), j6_pins["1"][1])
    sb.add_wire(j6_pins["2"][0], j6_pins["2"][1], j6_pins["2"][0] + g(14), j6_pins["2"][1])
    sb.place_power("+3V3", j6_pins["2"][0] + g(14), j6_pins["2"][1])
    for pnum, lbl in [("3", "I2C_SCL"), ("4", "I2C_SDA")]:
        px, py = j6_pins[pnum]
        sb.add_wire(px, py, px + g(8), py)
        sb.add_label(lbl, px + g(8), py, angle=0)

    # U3: MCP23017 SSCI-016506
    u3_sym = make_mcp23017_module()
    u3_pins = sb.place_symbol(u3_sym, "U3", "MCP23017_SSCI-016506_0x20", g(144), g(96))

    vx, vy = u3_pins["1"]
    sb.add_wire(vx, vy, vx - g(8), vy)
    sb.place_power("+3V3", vx - g(8), vy)

    gx, gy = u3_pins["2"]
    sb.add_wire(gx, gy, gx - g(8), gy)
    sb.place_power("GND", gx - g(8), gy)

    for pnum, lbl in [("3", "I2C_SCL"), ("4", "I2C_SDA")]:
        px, py = u3_pins[pnum]
        sb.add_wire(px, py, px - g(8), py)
        sb.add_label(lbl, px - g(8), py, angle=180)

    sw_sym = make_switch_push()
    b0x, b0y = u3_pins["5"]
    sw5_pins = sb.place_symbol(sw_sym, "SW5", "BTN_OLED", b0x - g(16), b0y,
                               ref_offset=(0, -g(3), 0), val_offset=(0, g(3), 0))
    sb.add_wire(b0x, b0y, sw5_pins["2"][0], sw5_pins["2"][1])
    sb.add_wire(sw5_pins["1"][0], sw5_pins["1"][1], sw5_pins["1"][0] - g(6), sw5_pins["1"][1])
    sb.place_power("GND", sw5_pins["1"][0] - g(6), sw5_pins["1"][1])

    for pnum in [str(i) for i in range(6, 13)]:
        sb.add_nc(*u3_pins[pnum])

    r_sym = make_resistor()
    led_sym = make_led()
    for ch in range(1, 5):
        pnum = str(12 + ch)
        ax, ay = u3_pins[pnum]
        r_cx = ax + g(14)
        r_pins = sb.place_symbol(r_sym, f"R{9+ch}", "R_LED_TBD[G6]", r_cx, ay, angle=90,
                                 ref_offset=(0, -g(3), 0), val_offset=(0, g(3), 0))
        sb.add_wire(ax, ay, r_pins["1"][0], r_pins["1"][1])
        d_cx = r_pins["2"][0] + g(14)
        d_pins = sb.place_symbol(led_sym, f"D{ch}", f"PANEL_LED_CH{ch}", d_cx, ay, angle=180,
                                 ref_offset=(0, -g(3), 0), val_offset=(0, g(3), 0))
        sb.add_wire(r_pins["2"][0], r_pins["2"][1], d_pins["2"][0], d_pins["2"][1])
        sb.add_wire(d_pins["1"][0], d_pins["1"][1], d_pins["1"][0] + g(6), d_pins["1"][1])
        sb.place_power("GND", d_pins["1"][0] + g(6), d_pins["1"][1])

    for ch in range(1, 5):
        pnum = str(16 + ch)
        bx, by = u3_pins[pnum]
        sw_cx = bx + g(16)
        sw_pins = sb.place_symbol(sw_sym, f"SW{ch}", f"BTN_CH{ch}", sw_cx, by,
                                  ref_offset=(0, -g(3), 0), val_offset=(0, g(3), 0))
        sb.add_wire(bx, by, sw_pins["1"][0], sw_pins["1"][1])
        sb.add_wire(sw_pins["2"][0], sw_pins["2"][1], sw_pins["2"][0] + g(6), sw_pins["2"][1])
        sb.place_power("GND", sw_pins["2"][0] + g(6), sw_pins["2"][1])

    sb.add_rect(g(14), g(158), g(220), g(214), width=0.254, color=(30, 80, 160, 1.0))
    sb.add_text(
        "SHARED I2C BUS & MCP23017 PANEL NOTES (docs/WIRING.md §6.3, HW_VERIFY G1-G9)\n"
        "- I2C Pull-Ups: ESP32-P4-ETH PCB has R44/R46 = 2.2kOhm -> ESP_3V3.\n"
        "  SSCI-016506 has R1/R2 unpopulated; verify OLED module PU & combined R [G3].\n"
        "- I2C Addresses: TC358743=0x0F [ok], MCP23017=0x20, SSD1306=0x3C/0x3D, ES8311=? [G2].\n"
        "- SSCI-016506 INTA/INTB/~RESET are NOT exposed on module header (internal 10k+0.1uF\n"
        "  on /RESET); polling operation only. SW1..SW5 use MCP23017 internal 100k pull-ups.\n"
        "- OLED-3 display policy: 30s auto-off, no I2C traffic when OFF, 1Hz dirty-page update.\n"
        "- NEVER route ATX (A0/A1/~E_*) or KVM LED truth inputs through MCP23017!",
        g(17), g(161), size=1.35, color=(20, 50, 120, 1.0)
    )

    return sb


def build_atx_kvm_sheet(root_uuid: str, block_uuid: str) -> SheetBuilder:
    sb = SheetBuilder(
        key="atx_kvm_ctrl",
        title="4ch ATX & KVM Switch Control (74HC139 x2 + Photorelays + Opto LED)",
        rev="1.0",
        page_num=5,
        hier_path=f"/{root_uuid}/{block_uuid}",
        comment_lines=[
            "docs/WIRING.md §6.4 & docs/PLAN.md §4 (Direct GPIO only; A0/A1 PD 100k, ~E PU 10k)",
            "TODO(hw-verify): Photorelay/Optocoupler PNs, R values, sink current [A1-A8, C1-C7]",
        ],
    )
    sb.add_text("Sheet 5: 4ch ATX & KVM Control (Direct GPIO + 74HC139 x2 + Isolated Drivers/Inputs)",
                g(14), g(12), size=2.2, bold=True)

    r_sym = make_resistor()
    c_sym = make_cap()
    dec_sym = make_74hc139()
    pr_sym = make_photorelay()
    opto_sym = make_optocoupler()

    # Fail-safe pull-downs (A0/A1 100k->GND) & pull-ups (~E_* 10k->+3V3) with staggered tap X for zero vertical overlap
    ctrl_inputs = [
        ("KVM_A0", "input", "R3", "100k", False, g(40)),
        ("KVM_A1", "input", "R4", "100k", False, g(46)),
        ("~{E_PWR}", "input", "R5", "10k", True, g(54)),
        ("~{E_RST}", "input", "R6", "10k", True, g(40)),
        ("~{E_KVM}", "input", "R7", "10k", True, g(52)),
    ]
    for idx, (hname, shape, rref, rval, pull_up, jx) in enumerate(ctrl_inputs):
        hy = g(26 + idx * 18)
        hx = g(28)
        sb.add_hlabel(hname, shape, hx, hy, angle=180)
        lx = g(64)
        sb.add_wire(hx, hy, jx, hy)
        sb.add_wire(jx, hy, lx, hy)
        sb.add_junction(jx, hy)
        sb.add_label(hname, lx, hy, angle=0)
        if pull_up:
            rcx, rcy = jx, hy - g(6)
            rp = sb.place_symbol(r_sym, rref, rval, rcx, rcy, angle=0,
                                 ref_offset=(g(3), -g(1), 0), val_offset=(g(3), g(1), 0))
            sb.add_wire(jx, hy, rp["2"][0], rp["2"][1])
            sb.add_wire(rp["1"][0], rp["1"][1], rp["1"][0], rp["1"][1] - g(2))
            sb.place_power("+3V3", rp["1"][0], rp["1"][1] - g(2))
        else:
            rcx, rcy = jx, hy + g(6)
            rp = sb.place_symbol(r_sym, rref, rval, rcx, rcy, angle=0,
                                 ref_offset=(g(3), -g(1), 0), val_offset=(g(3), g(1), 0))
            sb.add_wire(jx, hy, rp["1"][0], rp["1"][1])
            sb.add_wire(rp["2"][0], rp["2"][1], rp["2"][0], rp["2"][1] + g(2))
            sb.place_power("GND", rp["2"][0], rp["2"][1] + g(2))

    # U1 & U2 (74HC139 #1 and #2)
    u1_u1 = sb.place_symbol(dec_sym, "U1", "74HC139", g(96), g(34), unit=1)
    for pnum, lbl in [("2", "KVM_A0"), ("3", "KVM_A1"), ("1", "~{E_PWR}")]:
        px, py = u1_u1[pnum]
        sb.add_wire(px, py, px - g(6), py)
        sb.add_label(lbl, px - g(6), py, angle=180)
    for ch, pnum in enumerate(["4", "5", "6", "7"], start=1):
        px, py = u1_u1[pnum]
        sb.add_wire(px, py, px + g(6), py)
        sb.add_label(f"PWR_DRV_CH{ch}", px + g(6), py, angle=0)

    u1_u2 = sb.place_symbol(dec_sym, "U1", "74HC139", g(96), g(62), unit=2)
    for pnum, lbl in [("14", "KVM_A0"), ("13", "KVM_A1"), ("15", "~{E_RST}")]:
        px, py = u1_u2[pnum]
        sb.add_wire(px, py, px - g(6), py)
        sb.add_label(lbl, px - g(6), py, angle=180)
    for ch, pnum in enumerate(["12", "11", "10", "9"], start=1):
        px, py = u1_u2[pnum]
        sb.add_wire(px, py, px + g(6), py)
        sb.add_label(f"RST_DRV_CH{ch}", px + g(6), py, angle=0)

    u2_u1 = sb.place_symbol(dec_sym, "U2", "74HC139", g(96), g(90), unit=1)
    for pnum, lbl in [("2", "KVM_A0"), ("3", "KVM_A1"), ("1", "~{E_KVM}")]:
        px, py = u2_u1[pnum]
        sb.add_wire(px, py, px - g(6), py)
        sb.add_label(lbl, px - g(6), py, angle=180)
    for ch, pnum in enumerate(["4", "5", "6", "7"], start=1):
        px, py = u2_u1[pnum]
        sb.add_wire(px, py, px + g(6), py)
        sb.add_label(f"KVM_DRV_CH{ch}", px + g(6), py, angle=0)

    u2_u2 = sb.place_symbol(dec_sym, "U2", "74HC139", g(96), g(118), unit=2)
    p14x, p14y = u2_u2["14"]
    p13x, p13y = u2_u2["13"]
    gx = p14x - g(6)
    sb.add_wire(p14x, p14y, gx, p14y)
    sb.add_wire(gx, p14y, gx, p13y)
    sb.add_wire(p13x, p13y, gx, p13y)
    sb.add_junction(gx, p13y)
    sb.add_wire(gx, p13y, gx - g(4), p13y)
    sb.place_power("GND", gx - g(4), p13y)
    g2x, g2y = u2_u2["15"]
    sb.add_wire(g2x, g2y, g2x - g(6), g2y)
    sb.place_power("+3V3", g2x - g(6), g2y)
    for pnum in ["12", "11", "10", "9"]:
        sb.add_nc(*u2_u2[pnum])

    for uref, cref, cx in [("U1", "C1", g(76)), ("U2", "C2", g(104))]:
        uy = g(144)
        up = sb.place_symbol(dec_sym, uref, "74HC139", cx, uy, unit=3,
                             ref_offset=(-g(6), -g(1), 0), val_offset=(-g(6), g(2), 0))
        vx, vy = up["16"]
        gx, gy = up["8"]
        sb.add_wire(vx, vy, vx, vy - g(2))
        sb.place_power("+3V3", vx, vy - g(2))
        sb.add_wire(gx, gy, gx, gy + g(2))
        sb.place_power("GND", gx, gy + g(2))
        ccx = cx + g(14)
        cp = sb.place_symbol(c_sym, cref, "100nF", ccx, uy)
        sb.add_wire(cp["1"][0], cp["1"][1], cp["1"][0], cp["1"][1] - g(2))
        sb.place_power("+3V3", cp["1"][0], cp["1"][1] - g(2))
        sb.add_wire(cp["2"][0], cp["2"][1], cp["2"][0], cp["2"][1] + g(2))
        sb.place_power("GND", cp["2"][0], cp["2"][1] + g(2))

    # 3 Banks of 4 Photorelays positioned comfortably within x = 148..395 mm
    banks = [
        ("ATX POWER SW (CH1..4)", "PWR_DRV_CH", 1, g(164), "J7", "PWR"),
        ("ATX RESET SW (CH1..4)", "RST_DRV_CH", 5, g(222), "J8", "RST"),
        ("KVM BUTTON SW (CH1..4)", "KVM_DRV_CH", 9, g(280), "J9", "KVM"),
    ]
    j2p_sym = make_conn_1xN_left(
        "quad_ipkvm:Conn_Out_2P", 2, ["OUT_A", "OUT_B"], width_units=8, spacing_units=4,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"
    )
    for b_idx, (title_txt, drv_prefix, k_start, base_x, j_ref, tag) in enumerate(banks):
        sb.add_text(title_txt, base_x - g(16), g(18), size=1.4, bold=True)
        for ch in range(1, 5):
            k_num = k_start + (ch - 1)
            ky = g(32 + (ch - 1) * 20)
            k_pins = sb.place_symbol(pr_sym, f"K{k_num}", "PhotoMOS_TBD[C1]", base_x, ky,
                                     ref_offset=(0, -g(5), 0), val_offset=(0, g(5), 0))
            ax, ay = k_pins["1"]
            rp = sb.place_symbol(r_sym, f"R{20 + k_num}", "R_TBD[C1/C3]", ax - g(8), ay, angle=90,
                                 ref_offset=(0, -g(4), 0), val_offset=(0, -g(2), 0))
            sb.add_wire(rp["2"][0], rp["2"][1], ax, ay)
            sb.add_wire(rp["1"][0], rp["1"][1], rp["1"][0] - g(4), rp["1"][1])
            sb.place_power("+3V3", rp["1"][0] - g(4), rp["1"][1])

            kx, k_y = k_pins["2"]
            sb.add_wire(kx, k_y, kx - g(6), k_y)
            sb.add_label(f"{drv_prefix}{ch}", kx - g(6), k_y, angle=180)

            jp = sb.place_symbol(j2p_sym, f"{j_ref}_{ch}", f"{tag}_CH{ch}", base_x + g(16), ky,
                                 ref_offset=(0, -g(6), 0), val_offset=(0, g(6), 0))
            sb.add_wire(k_pins["3"][0], k_pins["3"][1], jp["1"][0], jp["1"][1])
            sb.add_wire(k_pins["4"][0], k_pins["4"][1], jp["2"][0], jp["2"][1])

    # 4x Isolated KVM LED Input Optocouplers (generous g(22) vertical spacing, x <= 265 so clear of title block!)
    sb.add_text("KVM LED ISOLATED INPUTS (4ch Truth State -> ESP32-P4 Direct GPIO)",
                g(126), g(116), size=1.4, bold=True)
    j2p_in_sym = make_conn_1xN(
        "quad_ipkvm:Conn_KVM_LED_2P", 2, ["LED+", "LED-"], width_units=8, spacing_units=4,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"
    )
    for ch in range(1, 5):
        oy = g(132 + (ch - 1) * 23)
        ox = g(168)
        ok_pins = sb.place_symbol(opto_sym, f"OK{ch}", "Opto_TBD[C2]", ox, oy,
                                  ref_offset=(0, -g(5), 0), val_offset=(0, g(5), 0))
        jp = sb.place_symbol(j2p_in_sym, f"J10_{ch}", f"KVM_LED_IN{ch}", ox - g(26), oy,
                             ref_offset=(0, -g(6), 0), val_offset=(0, g(6), 0))
        rp_in = sb.place_symbol(r_sym, f"R{32 + ch}", "R_TBD[A6/C2]", ox - g(12), ok_pins["1"][1], angle=90,
                                ref_offset=(0, -g(2), 0), val_offset=(0, g(2), 0))
        sb.add_wire(jp["1"][0], jp["1"][1], rp_in["1"][0], rp_in["1"][1])
        sb.add_wire(rp_in["2"][0], rp_in["2"][1], ok_pins["1"][0], ok_pins["1"][1])
        sb.add_wire(jp["2"][0], jp["2"][1], ok_pins["2"][0], ok_pins["2"][1])

        cx, cy = ok_pins["4"]
        jx = cx + g(14)
        sb.add_wire(cx, cy, jx, cy)
        sb.add_junction(jx, cy)
        sb.add_wire(jx, cy, jx + g(14), cy)
        sb.add_hlabel(f"KVM_LED_CH{ch}", "output", jx + g(14), cy, angle=0)
        rp_pu = sb.place_symbol(r_sym, f"R{36 + ch}", "10k_PU", jx, cy - g(7), angle=0,
                                ref_offset=(g(4), -g(1), 0), val_offset=(g(4), g(1), 0))
        sb.add_wire(jx, cy, rp_pu["2"][0], rp_pu["2"][1])
        sb.add_wire(rp_pu["1"][0], rp_pu["1"][1], rp_pu["1"][0], rp_pu["1"][1] - g(2))
        sb.place_power("+3V3", rp_pu["1"][0], rp_pu["1"][1] - g(2))

        ex, ey = ok_pins["3"]
        sb.add_wire(ex, ey, ex + g(4), ey)
        sb.place_power("GND", ex + g(4), ey)

    # Bottom-Left Notes box
    sb.add_rect(g(14), g(164), g(116), g(218), width=0.254, color=(150, 70, 0, 1.0))
    sb.add_text(
        "ATX/KVM SAFETY & TODO(hw-verify) NOTES (docs/WIRING.md §6.4)\n"
        "- Invariant 4: NEVER change KVM_A0/A1 while any ~E is Low.\n"
        "  Sequence: All ~E High -> change A0/A1 -> wait setup -> single ~E Low.\n"
        "- Fail-safe on ESP reset (High-Z): R5..R7 (10k PU) disable ALL 74HC139\n"
        "  outputs; R3/R4 (100k PD) hold A0/A1 = 00 (CH1).\n"
        "- Verify 74HC139 sink current vs photorelay If [C1/C3]; add NPN buffer if needed.\n"
        "- Verify KVM button common wire & LED voltage/polarity/PWM [A1..A8, C2].\n"
        "- Firmware disables internal GPIO PU/PD on LED pins; external 10k PU/PD mandatory!",
        g(17), g(167), size=1.3, color=(120, 50, 0, 1.0)
    )

    return sb


def build_project_json(root_uuid: str) -> dict:
    return {
        "board": {
            "3dviewsettings": {},
            "design_settings": {},
            "ipc2581": {},
            "layer_presets": [],
            "viewports": [],
        },
        "boards": [],
        "cvpcb": {"equivalence_files": []},
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": "quad-ipkvm.kicad_pro", "version": 1},
        "net_settings": {
            "classes": [
                {
                    "bus_width": 12,
                    "clearance": 0.2,
                    "diff_pair_gap": 0.25,
                    "diff_pair_via_gap": 0.25,
                    "diff_pair_width": 0.2,
                    "line_style": 0,
                    "microvia_diameter": 0.3,
                    "microvia_drill": 0.1,
                    "name": "Default",
                    "pcb_color": "rgba(0, 0, 0, 0.000)",
                    "schematic_color": "rgba(0, 0, 0, 0.000)",
                    "track_width": 0.25,
                    "via_diameter": 0.8,
                    "via_drill": 0.4,
                    "wire_width": 6,
                }
            ],
            "meta": {"version": 3},
            "net_colors": None,
            "netclass_assignments": None,
            "netclass_patterns": [],
        },
        "pcbnew": {"last_paths": {"gencad": "", "idf": "", "netlist": "", "specctra_dsn": "", "step": "", "vrml": ""}, "page_layout_descr_file": ""},
        "schematic": {
            "annotate_start_num": 0,
            "drawing": {
                "dashed_lines_dash_length_ratio": 12.0,
                "dashed_lines_gap_length_ratio": 3.0,
                "default_line_thickness": 6.0,
                "default_text_size": 50.0,
                "field_names": [],
                "intersheets_ref_own_page": False,
                "intersheets_ref_prefix": "",
                "intersheets_ref_short": False,
                "intersheets_ref_show": False,
                "intersheets_ref_suffix": "",
                "junction_size_choice": 3,
                "label_size_ratio": 0.375,
                "operating_point_overlay_i_precision": 3,
                "operating_point_overlay_i_range": "~A",
                "operating_point_overlay_v_precision": 3,
                "operating_point_overlay_v_range": "~V",
                "overbar_offset_ratio": 1.23,
                "pin_symbol_size": 25.0,
                "solder_multiplier": 1.5,
                "text_offset_ratio": 0.15,
            },
            "legacy_lib_dir": "",
            "legacy_lib_list": [],
            "meta": {"version": 1},
            "net_format_name": "",
            "page_layout_descr_file": "",
            "plot_directory": "",
            "spice_current_sheet_as_root": False,
            "spice_external_command": 'spice "%I"',
            "spice_model_current_sheet_as_root": True,
            "spice_save_all_currents": False,
            "spice_save_all_power": False,
            "spice_save_all_voltages": False,
            "subpart_first_id": 65,
            "subpart_id_separator": 0,
        },
        "sheets": [[root_uuid, ""]],
        "text_variables": {},
    }


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else repo_root / "hardware" / "kicad"
    out_dir.mkdir(parents=True, exist_ok=True)

    root_uuid = det_uuid("sheet:root")
    child_keys = ["usb_vbus", "x630_csi", "i2c_oled_panel", "atx_kvm_ctrl"]
    child_uuids = {k: det_uuid(f"block:{k}") for k in child_keys}

    sheets = {
        "quad-ipkvm.kicad_sch": (build_root_sheet(child_uuids), True),
        "usb_vbus.kicad_sch": (build_usb_vbus_sheet(root_uuid, child_uuids["usb_vbus"]), False),
        "x630_csi.kicad_sch": (build_x630_csi_sheet(root_uuid, child_uuids["x630_csi"]), False),
        "i2c_oled_panel.kicad_sch": (build_i2c_oled_sheet(root_uuid, child_uuids["i2c_oled_panel"]), False),
        "atx_kvm_ctrl.kicad_sch": (build_atx_kvm_sheet(root_uuid, child_uuids["atx_kvm_ctrl"]), False),
    }

    pro_path = out_dir / "quad-ipkvm.kicad_pro"
    pro_path.write_text(json.dumps(build_project_json(root_uuid), indent=2) + "\n", encoding="utf-8")

    for fname, (sb, is_root) in sheets.items():
        (out_dir / fname).write_text(sb.render_sch(is_root=is_root), encoding="utf-8")
        print(f"Generated {out_dir / fname} ({len(sb.items)} items, {len(sb.lib_defs)} lib_symbols)")

    print(f"Generated {pro_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
