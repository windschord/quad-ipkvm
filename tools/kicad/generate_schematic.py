#!/usr/bin/env python3
"""
Deterministic KiCad 8/9 single-sheet A2 schematic generator for quad-ipkvm.

Generates hardware/kicad/:
  - quad-ipkvm.kicad_pro     (KiCad 8/9 project file)
  - quad-ipkvm.kicad_sch     (Unified A2 single-sheet schematic with direct wiring)
  - quad_ipkvm.kicad_sym     (Project symbol library for all custom quad_ipkvm:* parts)
  - sym-lib-table            (Project symbol library table referencing ${KIPRJMOD})

Based on docs/WIRING.md (owner decisions: PR #178 R1=120k/R2=150k 1% E24 VBUS divider
(0.5% E96 130k/162k is the alternative), no J3 ESD diode) and the owner's hand-drawn ATX/KVM
circuit of 2026-11-28 (Issue #180: KVM button/LED direct GPIO, PC PWR/HDD LED sense via 74HC4052).

KiCad coordinate conventions enforced:
- Inside lib_symbols / .kicad_sym: Y-UP coordinate space.
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

    def to_sexpr(self, indent: str = "    ", use_short_name: bool = False) -> str:
        """Serialize symbol definition. Inside .kicad_sym, top name is unprefixed (short_name)."""
        lines: List[str] = []
        top_name = self.short_name if use_short_name else self.lib_id
        pn_hide = " hide" if self.hide_pin_names else ""
        pnum_hide = " (pin_numbers hide)" if self.hide_pin_numbers else ""
        pwr = " (power)" if self.is_power else ""
        in_bom = "yes" if self.in_bom else "no"
        on_board = "yes" if self.on_board else "no"
        ref_hide = " hide" if self.is_power else ""
        lines.append(
            f'{indent}(symbol {q(top_name)}{pwr}{pnum_hide}'
            f' (pin_names (offset {fmt(self.pin_names_offset)}){pn_hide})'
            f' (exclude_from_sim no) (in_bom {in_bom}) (on_board {on_board})'
        )
        rx, ry, ra = self.ref_pos
        vx, vy, va = self.val_pos
        i2 = indent + "  "
        i3 = indent + "    "
        lines.append(
            f'{i2}(property "Reference" {q(self.ref_prefix)} (at {fmt(rx)} {fmt(ry)} {ra})'
            f' (effects (font (size 1.27 1.27)){ref_hide}))'
        )
        lines.append(
            f'{i2}(property "Value" {q(self.value)} (at {fmt(vx)} {fmt(vy)} {va})'
            f' (effects (font (size 1.27 1.27))))'
        )
        lines.append(
            f'{i2}(property "Footprint" {q(self.footprint)} (at 0 0 0)'
            f' (effects (font (size 1.27 1.27)) hide))'
        )
        lines.append(
            f'{i2}(property "Datasheet" "~" (at 0 0 0)'
            f' (effects (font (size 1.27 1.27)) hide))'
        )

        g0 = self.graphics_by_unit.get(0, [])
        if g0:
            lines.append(f'{i2}(symbol {q(f"{self.short_name}_0_1")}')
            for item in g0:
                lines.append(f"{i3}{item}")
            lines.append(f"{i2})")

        for u in range(1, self.unit_count + 1):
            gu = self.graphics_by_unit.get(u, [])
            pu = [p for p in self.pins if p.unit == u]
            if not gu and not pu:
                continue
            lines.append(f'{i2}(symbol {q(f"{self.short_name}_{u}_1")}')
            for item in gu:
                lines.append(f"{i3}{item}")
            for p in pu:
                phide = " hide" if p.hide else ""
                lines.append(
                    f'{i3}(pin {p.etype} line (at {fmt(p.x)} {fmt(p.y)} {p.angle})'
                    f' (length {fmt(p.length)}){phide}'
                    f' (name {q(p.name)} (effects (font (size 1.016 1.016))))'
                    f' (number {q(p.number)} (effects (font (size 1.016 1.016))))'
                    f")"
                )
            lines.append(f"{i2})")
        lines.append(f"{indent})")
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
    """Switch Science SSCI-016506 MCP23017 3.3V module."""
    half_w = g(12)
    pin_len = g(4)
    lx = round(-half_w - pin_len, 2)
    rx = round(half_w + pin_len, 2)
    box_top = g(32)
    box_bot = g(-32)

    left_pins = [
        ("1", "VDD", "power_in", g(28)),
        ("2", "VSS", "power_in", g(22)),
        ("3", "SCL", "input", g(14)),
        ("4", "SDA", "bidirectional", g(8)),
        ("5", "GPB0", "bidirectional", g(-4)),
        ("6", "GPB1", "bidirectional", g(-10)),
        ("7", "GPB2", "bidirectional", g(-13)),
        ("8", "GPB3", "bidirectional", g(-16)),
        ("9", "GPB4", "bidirectional", g(-19)),
        ("10", "GPB5", "bidirectional", g(-22)),
        ("11", "GPB6", "bidirectional", g(-25)),
        ("12", "GPB7", "bidirectional", g(-28)),
    ]
    right_pins = [
        ("13", "GPA0", "bidirectional", g(28)),
        ("14", "GPA1", "bidirectional", g(20)),
        ("15", "GPA2", "bidirectional", g(12)),
        ("16", "GPA3", "bidirectional", g(4)),
        ("17", "GPA4", "bidirectional", g(-4)),
        ("18", "GPA5", "bidirectional", g(-12)),
        ("19", "GPA6", "bidirectional", g(-20)),
        ("20", "GPA7", "bidirectional", g(-28)),
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
        ref_pos=(0.0, g(34), 0),
        val_pos=(0.0, g(-34), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-half_w)} {fmt(box_top)}) (end {fmt(half_w)} {fmt(box_bot)})"
                " (stroke (width 0.254) (type default)) (fill (type background)))",
            ]
        },
        pins=pins,
    )


def make_74hc139() -> LibSymDef:
    """Dual 2-to-4 line decoder/demultiplexer 74HC139 (project custom quad_ipkvm:74HC139, DIP-16/SOIC-16, 3 units)."""
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
        lib_id="quad_ipkvm:74HC139",
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


def make_optocoupler_left_out() -> LibSymDef:
    """
    4-pin DC Optocoupler (PC817 / LTV-817 candidate — TODO(hw-verify) C2) oriented for signal flow
    RIGHT-TO-LEFT toward ESP32-P4 P1:
      Left side (facing P1):  4=C (open_collector), 3=E (passive — ERC-clean with GND!)
      Right side (facing KVM): 1=A (passive), 2=K (passive)
    """
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
                # Phototransistor bar on left inside box
                "(polyline (pts (xy -1.778 1.524) (xy -1.778 -1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
                # LED triangle on right inside box
                "(polyline (pts (xy 1.016 1.524) (xy 2.54 1.524) (xy 1.778 0.254) (xy 1.016 1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("4", "C", "open_collector", lx, g(2), 0, length=pin_len),
            PinDef("3", "E", "passive", lx, g(-2), 0, length=pin_len),
            PinDef("1", "A", "passive", rx, g(2), 180, length=pin_len),
            PinDef("2", "K", "passive", rx, g(-2), 180, length=pin_len),
        ],
    )


def make_photorelay_mirror() -> LibSymDef:
    """
    Photorelay (G3VM-61A1 class) mirrored: input LED (A/K) on the RIGHT, MOSFET output (S1/S2) on the LEFT.
    Used for the PC front-panel PWR/HDD LED sense blocks placed to the right of the 74HC4052.
    """
    hw = g(4)
    hh = g(3)
    pin_len = g(2)
    lx = round(-hw - pin_len, 2)
    rx = round(hw + pin_len, 2)
    return LibSymDef(
        lib_id="quad_ipkvm:Photorelay_SPST_M",
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
                "(polyline (pts (xy 2.54 1.524) (xy 1.016 1.524) (xy 1.778 0.254) (xy 2.54 1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
                "(polyline (pts (xy -1.778 1.524) (xy -2.54 0) (xy -1.778 -1.524)) (stroke (width 0.2) (type default)) (fill (type none)))",
            ]
        },
        pins=[
            PinDef("1", "A", "passive", rx, g(2), 180, length=pin_len),
            PinDef("2", "K", "passive", rx, g(-2), 180, length=pin_len),
            PinDef("3", "S1", "passive", lx, g(2), 0, length=pin_len),
            PinDef("4", "S2", "passive", lx, g(-2), 0, length=pin_len),
        ],
    )


def make_74hc4052() -> LibSymDef:
    """
    Dual 4-channel analog multiplexer 74HC4052 (project custom quad_ipkvm:74HC4052, DIP-16/SOIC-16).
    Drawn stretched (channel pins 16 grid units apart) so that each relay row wires straight into its channel pin.
    Pin numbers follow the common 74HC4052 pinout [TODO(hw-verify): confirm with the chosen manufacturer datasheet].
    Single 3.3 V supply: VEE and ~E are tied to GND (always enabled).
    """
    hw = g(12)
    pin_len = g(3)
    lx = round(-hw - pin_len, 2)
    rx = round(hw + pin_len, 2)
    box_top = g(28)
    box_bot = g(-28)
    ch_y = [g(24), g(8), g(-8), g(-24)]
    pins: List[PinDef] = []
    for (num, name), y in zip([("12", "1Y0"), ("14", "1Y1"), ("15", "1Y2"), ("11", "1Y3")], ch_y):
        pins.append(PinDef(num, name, "passive", lx, y, 0, length=pin_len))
    for (num, name), y in zip([("1", "2Y0"), ("5", "2Y1"), ("2", "2Y2"), ("4", "2Y3")], ch_y):
        pins.append(PinDef(num, name, "passive", rx, y, 180, length=pin_len))
    for num, name, etype, x in [
        ("10", "S0(A)", "input", g(-8)),
        ("9", "S1(B)", "input", g(-4)),
        ("16", "VCC", "power_in", 0.0),
        ("13", "1Z", "passive", g(4)),
        ("3", "2Z", "passive", g(8)),
    ]:
        pins.append(PinDef(num, name, etype, x, g(32), 270, length=g(4)))
    for num, name, etype, x in [
        ("6", "~{E}", "input", g(-4)),
        ("7", "VEE", "power_in", 0.0),
        ("8", "GND", "power_in", g(4)),
    ]:
        pins.append(PinDef(num, name, etype, x, g(-32), 90, length=g(4)))
    return LibSymDef(
        lib_id="quad_ipkvm:74HC4052",
        ref_prefix="U",
        value="74HC4052",
        footprint="Package_DIP:DIP-16_W7.62mm",
        pin_names_offset=0.762,
        ref_pos=(0.0, g(36), 0),
        val_pos=(0.0, g(-36), 0),
        graphics_by_unit={
            0: [
                f"(rectangle (start {fmt(-hw)} {fmt(box_top)}) (end {fmt(hw)} {fmt(box_bot)})"
                " (stroke (width 0.254) (type default)) (fill (type background)))",
            ]
        },
        pins=pins,
    )


# ---------------------------------------------------------------------------
# Sheet Builder
# ---------------------------------------------------------------------------

class SheetBuilder:
    def __init__(self, key: str, title: str, rev: str, hier_path: str, comment_lines: List[str]):
        self.key = key
        self.uuid = det_uuid(f"sheet:{key}")
        self.title = title
        self.rev = rev
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

    def add_wire_poly(self, pts: List[Tuple[float, float]]) -> None:
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            self.add_wire(x1, y1, x2, y2)

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
        x, y = round(x, 2), round(y, 2)
        u = det_uuid(f"{self.key}:lbl:{len(self.items)}:{name}:{x}:{y}:{angle}")
        just = "right bottom" if angle == 180 else "left bottom"
        self.items.append(
            f'  (label {q(name)} (at {fmt(x)} {fmt(y)} {angle}) (fields_autoplaced yes)\n'
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
                 stroke_type: str = "solid", color: Optional[Tuple[int, int, int, float]] = None) -> None:
        u = det_uuid(f"{self.key}:rect:{len(self.items)}:{x1}:{y1}:{x2}:{y2}")
        c_str = f" (color {color[0]} {color[1]} {color[2]} {color[3]})" if color else ""
        self.items.append(
            f'  (rectangle (start {fmt(x1)} {fmt(y1)}) (end {fmt(x2)} {fmt(y2)})\n'
            f'    (stroke (width {fmt(width)}) (type {stroke_type}){c_str}) (fill (type none))\n'
            f'    (uuid {q(u)})\n'
            f'  )'
        )

    def render_sch(self) -> str:
        c_lines = "\n".join(f'    (comment {i + 1} {q(c)})' for i, c in enumerate(self.comment_lines[:4]))
        lib_block = "\n".join(self.lib_defs[k].to_sexpr() for k in sorted(self.lib_defs))
        body = "\n".join(self.items)
        return (
            f'(kicad_sch\n'
            f'  (version {KICAD_VERSION})\n'
            f'  (generator {q(GENERATOR_NAME)})\n'
            f'  (generator_version "1.0")\n'
            f'  (uuid {q(self.uuid)})\n'
            f'  (paper "A2")\n'
            f'  (title_block\n'
            f'    (title {q(self.title)})\n'
            f'    (date "2026-11-28")\n'
            f'    (rev {q(self.rev)})\n'
            f'    (company "quad-ipkvm")\n'
            f'{c_lines}\n'
            f'  )\n'
            f'  (lib_symbols\n'
            f'{lib_block}\n'
            f'  )\n'
            f'{body}\n'
            f'  (sheet_instances\n'
            f'    (path "/" (page "1"))\n'
            f'  )\n'
            f')\n'
        )


# ---------------------------------------------------------------------------
# Unified A2 Schematic Layout (Direct Continuous Inter-Module Wiring)
# ---------------------------------------------------------------------------

def build_unified_sheet() -> SheetBuilder:
    root_uuid = det_uuid("sheet:root")
    sb = SheetBuilder(
        key="root",
        title="quad-ipkvm Full System Schematic (ESP32-P4-ETH rev v1.3)",
        rev="1.1",
        hier_path=f"/{root_uuid}",
        comment_lines=[
            "Primary reference: docs/WIRING.md & docs/ESP32-P4-ETH-schematic.pdf",
            "PR #178: VBUS R1=120k/R2=150k 1% E24 (alt 130k/162k 0.5% E96), NO J3 ESD diode",
            "Safety: NEVER connect J3 #1 (VCC_5V) to PC VBUS; NEVER connect X630 #2 HDET (4.8V)",
            "Issue #180: KVM button/LED = direct GPIO (3.3V), PC PWR/HDD LED sense = 74HC4052. GPIOs PROVISIONAL",
        ],
    )

    sb.add_text(
        "quad-ipkvm Unified Hardware Schematic (Waveshare ESP32-P4-ETH rev v1.3 + X630 V1.5 + USB VBUS + OLED/MCP23017 + 4ch ATX/KVM)",
        g(12), g(10), size=2.4, bold=True
    )

    # ==================================================================
    # Zone Borders (clean dashed frames dividing the 5 functional areas)
    # ==================================================================
    # Center Hub: ESP32-P4-ETH (J3 + P1)
    sb.add_rect(g(144), g(16), g(204), g(210), width=0.254, stroke_type="dash", color=(40, 90, 170, 1.0))
    sb.add_text("ESP32-P4-ETH rev v1.3 (HOST BOARD)", g(147), g(18), size=1.6, bold=True, color=(30, 80, 160, 1.0))

    # Zone 1: Top-Left USB OTG + VBUS
    sb.add_rect(g(12), g(16), g(138), g(70), width=0.254, stroke_type="dash", color=(20, 120, 50, 1.0))
    sb.add_text("1. TARGET PC USB OTG HS & VBUS SENSE (PR #178: R1=120k/R2=150k 1% E24, NO ESD)", g(15), g(18), size=1.5, bold=True, color=(20, 110, 40, 1.0))

    # Zone 2: Mid-Left Geekworm X630
    sb.add_rect(g(12), g(74), g(138), g(204), width=0.254, stroke_type="dash", color=(20, 120, 50, 1.0))
    sb.add_text("2. GEEKWORM X630 V1.5 (TC358743XBG) MIPI-CSI & 10-PIN CTRL", g(15), g(76), size=1.5, bold=True, color=(20, 110, 40, 1.0))

    # Zone 3: Bottom-Left Shared I2C OLED & MCP23017
    sb.add_rect(g(12), g(208), g(204), g(318), width=0.254, stroke_type="dash", color=(180, 90, 0, 1.0))
    sb.add_text("3. SHARED I2C BUS (GPIO7/8) — SSD1306 OLED & MCP23017 PANEL", g(15), g(210), size=1.5, bold=True, color=(150, 70, 0, 1.0))

    # Zone 4: Top/Mid-Right 4ch ATX/KVM Control
    sb.add_rect(g(216), g(16), g(456), g(204), width=0.254, stroke_type="dash", color=(110, 40, 160, 1.0))
    sb.add_text("4. ATX PWR/RST OUTPUT (A,B + ~E -> 74HC139 x1 -> 8x Photorelay) & KVM DIRECT GPIO I/O (Issue #180) [TODO(hw-verify)]", g(219), g(18), size=1.5, bold=True, color=(90, 30, 140, 1.0))

    # Zone 5: Bottom-Mid-Right KVM LED Opto Inputs
    sb.add_rect(g(216), g(208), g(360), g(318), width=0.254, stroke_type="dash", color=(110, 40, 160, 1.0))
    sb.add_text("5. PC FRONT-PANEL PWR/HDD LED SENSE (8x Photorelay -> 74HC4052 mux -> 2 GPIO) (Issue #180) [TODO(hw-verify)]", g(219), g(210), size=1.5, bold=True, color=(90, 30, 140, 1.0))

    # ==================================================================
    # CENTER HUB: J3 (USB OTG HS) & P1 (40-pin Header)
    # ==================================================================
    j3_sym = make_conn_1xN_left(
        "quad_ipkvm:ESP32-P4-ETH_J3",
        4,
        ["VCC_5V (NC!)", "USBD_N", "USBD_P", "GND"],
        ["passive", "bidirectional", "bidirectional", "power_in"],
        width_units=16,
        spacing_units=4,
        footprint="Connector_JST:JST_SH_BM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal",
    )
    j3_pins = sb.place_symbol(j3_sym, "J3", "USB_OTG_HS_4P", g(168), g(36))
    sb.add_nc(*j3_pins["1"])
    sb.add_text("FORBIDDEN: DO NOT CONNECT TO PC VBUS [H3]!", g(150), g(23), size=1.2, bold=True, color=(200, 0, 0, 1.0))
    sb.add_wire(j3_pins["4"][0], j3_pins["4"][1], j3_pins["4"][0] - g(4), j3_pins["4"][1])
    sb.place_power("GND", j3_pins["4"][0] - g(4), j3_pins["4"][1])

    p1_sym = make_p1_40pin()
    p1_x, p1_y = g(174), g(120)
    p1_pins = sb.place_symbol(p1_sym, "P1", "ESP32-P4-ETH_P1_2x20", p1_x, p1_y)
    # Note: left pins at x = g(154), right pins at x = g(194), y spans g(82)..g(158)

    # P1 Left Unused / JTAG / Fixed
    for pin_num in ["11", "12", "14", "15", "19", "20"]:
        sb.add_nc(*p1_pins[pin_num])
    # Former spare GPIOs (#1,#2,#7,#9,#10) now carry the Issue #180 signals (PROVISIONAL).
    for pin_num, lname in [("1", "PC_HDD_L"), ("2", "KVM_BTN2"), ("7", "KVM_BTN3"), ("9", "KVM_BTN4"), ("10", "PC_PWR_L")]:
        px, py = p1_pins[pin_num]
        stub = g(8) if pin_num == "9" else g(4)  # #9 sits right below the #8 GND symbol text
        sb.add_wire(px, py, px - stub, py)
        sb.add_label(lname, px - stub, py, angle=180)
    sb.add_text("JTAG(NO OUT)", g(145), p1_pins["11"][1] - g(1), size=1.0, color=(150, 90, 0, 1.0))
    sb.add_text("USB_JTAG", g(146), p1_pins["19"][1] - g(1), size=1.0, color=(30, 80, 160, 1.0))

    # P1 Left GNDs (3, 8, 13, 18) + single PWR_FLAG on GND (CodeRabbit fix: PWR_FLAG on GND only)
    for idx, pin_num in enumerate(["3", "8", "13", "18"]):
        px, py = p1_pins[pin_num]
        gx = px - g(4)
        sb.add_wire(px, py, gx, py)
        sb.place_power("GND", gx, py)
        if idx == 0:
            fx = gx - g(10)
            sb.add_wire(gx, py, fx, py)
            sb.add_junction(gx, py)
            sb.place_power("PWR_FLAG", fx, py)

    # P1 Right Unused / Fixed
    for pin_num in ["40", "39", "37", "30", "29", "27"]:
        sb.add_nc(*p1_pins[pin_num])
    sb.add_text("H2 ONLY (NO EXT 5V)", p1_pins["40"][0] + g(2), p1_pins["40"][1] - g(1), size=1.05, color=(190, 20, 20, 1.0))
    sb.add_text("USB_FS", p1_pins["29"][0] + g(2), p1_pins["29"][1] - g(1), size=1.05, color=(30, 80, 160, 1.0))

    # P1 Right GNDs (38, 33, 28, 23)
    for pin_num in ["38", "33", "28", "23"]:
        px, py = p1_pins[pin_num]
        gx = px + g(4)
        sb.add_wire(px, py, gx, py)
        sb.place_power("GND", gx, py)

    # P1 #36 (+3V3 power_out -> +3V3 ONLY, NO PWR_FLAG so ERC has no duplicate power_out driver!)
    p36x, p36y = p1_pins["36"]
    sb.add_wire(p36x, p36y, p36x + g(5), p36y)
    sb.place_power("+3V3", p36x + g(5), p36y)
    sb.add_text("meas 3.34V [V2]", p36x + g(7), p36y - g(1), size=1.05)

    # P1 right GPIOs used by Issue #180 (KVM button #1 and KVM LED inputs): short stubs + net labels
    for pin_num, lname in [("24", "KVM_BTN1"), ("26", "KVM_LED1"), ("25", "KVM_LED2"), ("22", "KVM_LED3"), ("21", "KVM_LED4")]:
        px, py = p1_pins[pin_num]
        stub = g(8) if pin_num == "22" else g(4)  # #22 sits right below the #23 GND symbol text
        sb.add_wire(px, py, px + stub, py)
        sb.add_label(lname, px + stub, py, angle=0)

    # ==================================================================
    # ZONE 1: J2 Target PC USB + VBUS Sense Divider -> DIRECTLY wired to J3 & P1 #4
    # ==================================================================
    j2_sym = make_conn_1xN(
        "quad_ipkvm:Target_PC_USB_4P",
        4,
        ["VBUS_PC(5V)", "D-", "D+", "GND"],
        ["passive", "bidirectional", "bidirectional", "power_in"],
        width_units=16,
        spacing_units=4,
        footprint="Connector_USB:USB_A_Molex_67643_Horizontal",
    )
    j2_pins = sb.place_symbol(j2_sym, "J2", "Target_PC_USB_4P", g(32), g(36))
    # J2 pins exit RIGHT at x = g(44), y = [30, 34, 38, 42] — EXACTLY matching J3 y = [30, 34, 38, 42]!
    sb.add_wire(j2_pins["4"][0], j2_pins["4"][1], j2_pins["4"][0] + g(4), j2_pins["4"][1])
    sb.place_power("GND", j2_pins["4"][0] + g(4), j2_pins["4"][1])

    # Direct wires J2 #2/#3 -> J3 #2/#3 (purely horizontal, no detours!)
    for pnum, lname in [("2", "USB_DM"), ("3", "USB_DP")]:
        x2, y2 = j2_pins[pnum]
        x3, y3 = j3_pins[pnum]
        sb.add_wire(x2, y2, x3, y3)
        sb.add_label(lname, g(120), y2, angle=0)

    # VBUS divider: J2 #1 (g(44), g(30)) -> UP to y=g(24) -> R1 (g(68), g(24)) -> mid (g(86), g(24))
    # -> R2 hangs DOWN cleanly inside Zone 1 (y=g(34)..g(46), well left of any labels!) -> GND at (g(86), g(46))
    # -> VBUS_SENSE wire runs from mid (g(86), g(24)) RIGHT to g(142), DOWN to y=g(94) (P1 #4 level), RIGHT to P1 #4!
    r_sym = make_resistor()
    v1x, v1y = j2_pins["1"]
    top_vbus_y = g(24)
    sb.add_wire_poly([(v1x, v1y), (v1x, top_vbus_y)])
    r1_pins = sb.place_symbol(r_sym, "R1", "120k_1%_E24", g(68), top_vbus_y, angle=90,
                              ref_offset=(0, -g(3), 0), val_offset=(0, g(3), 0))
    sb.add_wire(v1x, top_vbus_y, r1_pins["1"][0], r1_pins["1"][1])

    mid_x = g(86)
    sb.add_wire(r1_pins["2"][0], r1_pins["2"][1], mid_x, top_vbus_y)
    sb.add_junction(mid_x, top_vbus_y)

    r2_pins = sb.place_symbol(r_sym, "R2", "150k_1%_E24", mid_x, g(46), angle=0,
                              ref_offset=(g(4), -g(1), 0), val_offset=(g(4), g(1), 0))
    sb.add_wire(mid_x, top_vbus_y, r2_pins["1"][0], r2_pins["1"][1])
    sb.add_wire(r2_pins["2"][0], r2_pins["2"][1], mid_x, g(54))
    sb.place_power("GND", mid_x, g(54))

    # Direct VBUS_SENSE wire to P1 #4 (GPIO18)
    p4x, p4y = p1_pins["4"]
    vs_drop_x = g(141)
    sb.add_wire_poly([(mid_x, top_vbus_y), (vs_drop_x, top_vbus_y), (vs_drop_x, p4y), (p4x, p4y)])
    sb.add_label("VBUS_SENSE", g(108), top_vbus_y, angle=0)

    sb.add_text(
        "Ratio=0.5556 (1% parts: 0.550-0.561) -> 4.75V: >=2.61V, 5.50V: <=3.09V (VDD meas. 3.34V [V2]). Power H2 BEFORE PC!",
        g(15), g(63), size=1.1, color=(20, 90, 40, 1.0)
    )

    # ==================================================================
    # ZONE 2: Geekworm X630 V1.5 (J4 15p <-> J1 22p Direct FFC + J5 10p -> P1)
    # ==================================================================
    j4_names = [
        "1:3V3", "2:SDA", "3:SCL", "4:REFCLK?", "5:IO0?",
        "6:GND", "7:CK_P", "8:CK_N", "9:GND", "10:D1_P",
        "11:D1_N", "12:GND", "13:D0_P", "14:D0_N", "15:GND"
    ]
    j4_sym = make_conn_1xN(
        "quad_ipkvm:X630_CSI_15P", 15, j4_names,
        width_units=14, spacing_units=3,
        footprint="Connector_FFC-FPC:Molex_52207-1585_1x15-1MP_P1.00mm_Horizontal"
    )
    j4_pins = sb.place_symbol(j4_sym, "J4", "X630_CSI_15P", g(23), g(108))

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
    j1_sym = make_conn_1xN_left(
        "quad_ipkvm:ESP32-P4-ETH_J1_CSI_22P", 22, j1_names, j1_types,
        width_units=14, spacing_units=3,
        footprint="Connector_FFC-FPC:Hirose_FH12-22S-0.5SH_1x22-1MP_P0.50mm_Horizontal"
    )
    j1_pins = sb.place_symbol(j1_sym, "J1", "ESP_J1_CSI_22P", g(110), g(119))

    for p in ["4", "7", "8", "9", "10", "11", "12"]:
        sb.add_nc(*j1_pins[p])

    # Shared +3V3 on J4 #1 & J1 #1
    sb.add_wire(j4_pins["1"][0], j4_pins["1"][1], j4_pins["1"][0] + g(5), j4_pins["1"][1])
    sb.place_power("+3V3", j4_pins["1"][0] + g(5), j4_pins["1"][1])
    sb.add_wire(j1_pins["1"][0], j1_pins["1"][1], j1_pins["1"][0] - g(5), j1_pins["1"][1])
    sb.place_power("+3V3", j1_pins["1"][0] - g(5), j1_pins["1"][1])

    # Direct staircase wires J4 <-> J1 for all signal pairs (unique step_x right of I2C trunk g(48)/g(52))
    ffc_direct = [
        ("2", "2", "I2C_SDA"),
        ("3", "3", "I2C_SCL"),
        ("4", "5", "CSI_IO1"),
        ("5", "6", "CSI_IO0"),
        ("7", "14", "CSI_CLK_P"),
        ("8", "15", "CSI_CLK_N"),
        ("10", "17", "CSI_D1_P"),
        ("11", "18", "CSI_D1_N"),
        ("13", "20", "CSI_D0_P"),
        ("14", "21", "CSI_D0_N"),
    ]
    for idx, (p4, p1, nname) in enumerate(ffc_direct):
        if nname in ("I2C_SDA", "I2C_SCL"):
            continue
        x4, y4 = j4_pins[p4]
        x1, y1 = j1_pins[p1]
        step_x = g(56 + (idx - 2) * 3)  # g(56)..g(77) — clean gap right of I2C trunk & left of J1 GND bus
        sb.add_wire_poly([(x4, y4), (step_x, y4), (step_x, y1), (x1, y1)])
        sb.add_label(nname, g(92), y1, angle=180)

    # GND buses on J4 and J1
    j4_gnds = [j4_pins[p] for p in ["6", "9", "12", "15"]]
    j4_gx = g(40)
    for idx, (px, py) in enumerate(j4_gnds):
        sb.add_wire(px, py, j4_gx, py)
        if idx > 0:
            sb.add_wire(j4_gx, j4_gnds[idx - 1][1], j4_gx, py)
            sb.add_junction(j4_gx, py)
    sb.add_wire(j4_gx, j4_gnds[-1][1], j4_gx, j4_gnds[-1][1] + g(3))
    sb.place_power("GND", j4_gx, j4_gnds[-1][1] + g(3))

    j1_gnds = [j1_pins[p] for p in ["13", "16", "19", "22"]]
    j1_gx = g(95)
    for idx, (px, py) in enumerate(j1_gnds):
        sb.add_wire(px, py, j1_gx, py)
        if idx > 0:
            sb.add_wire(j1_gx, j1_gnds[idx - 1][1], j1_gx, py)
            sb.add_junction(j1_gx, py)
    sb.add_wire(j1_gx, j1_gnds[-1][1], j1_gx, j1_gnds[-1][1] + g(3))
    sb.place_power("GND", j1_gx, j1_gnds[-1][1] + g(3))

    # J5 X630 10-pin SH1.0 Connector (bottom-left of Zone 2)
    j5_names = [
        "1:3V3_OUT", "2:HDET_4.8V!", "3:IR", "4:INT", "5:RESET_N",
        "6:A_SCK", "7:A_WFS", "8:A_OSCK", "9:A_SD", "10:GND"
    ]
    j5_sym = make_conn_1xN(
        "quad_ipkvm:X630_10P_SH1", 10, j5_names,
        width_units=16, spacing_units=3,
        footprint="Connector_JST:JST_SH_BM10B-SRSS-TB_1x10-1MP_P1.00mm_Horizontal"
    )
    j5_pins = sb.place_symbol(j5_sym, "J5", "X630_10P_SH1.0", g(28), g(176))
    for p in ["1", "2", "3", "6", "7", "8", "9"]:
        sb.add_nc(*j5_pins[p])
    sb.add_text("FORBIDDEN: 4.8V!", j5_pins["2"][0] + g(2), j5_pins["2"][1] - g(1), size=1.15, bold=True, color=(200, 0, 0, 1.0))
    sb.add_wire(j5_pins["10"][0], j5_pins["10"][1], j5_pins["10"][0] + g(4), j5_pins["10"][1])
    sb.place_power("GND", j5_pins["10"][0] + g(4), j5_pins["10"][1])

    # Direct wires: J5 #4 (INT) -> P1 #5 (GPIO17); J5 #5 (RESET) -> P1 #6 (GPIO16)
    j5_4x, j5_4y = j5_pins["4"]
    p5x, p5y = p1_pins["5"]
    sb.add_wire_poly([(j5_4x, j5_4y), (g(132), j5_4y), (g(132), p5y), (p5x, p5y)])
    sb.add_label("X630_INT[B6]", g(92), j5_4y, angle=0)

    j5_5x, j5_5y = j5_pins["5"]
    p6x, p6y = p1_pins["6"]
    sb.add_wire_poly([(j5_5x, j5_5y), (g(128), j5_5y), (g(128), p6y), (p6x, p6y)])
    sb.add_label("X630_RESET_N", g(92), j5_5y, angle=0)

    # ==================================================================
    # ZONE 3: Shared I2C Bus -> SSD1306 OLED (J6) & MCP23017 (U3)
    # ==================================================================
    j6_sym = make_conn_1xN(
        "quad_ipkvm:SSD1306_OLED_4P", 4,
        ["GND", "VCC(+3V3)", "SCL", "SDA"],
        ["power_in", "power_in", "input", "bidirectional"],
        width_units=16, spacing_units=6,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical"
    )
    j6_pins = sb.place_symbol(j6_sym, "J6", "SSD1306_OLED_0x3C", g(22), g(236))
    sb.add_wire(j6_pins["1"][0], j6_pins["1"][1], j6_pins["1"][0] + g(3), j6_pins["1"][1])
    sb.place_power("GND", j6_pins["1"][0] + g(3), j6_pins["1"][1])
    sb.add_wire(j6_pins["2"][0], j6_pins["2"][1], j6_pins["2"][0] + g(8), j6_pins["2"][1])
    sb.place_power("+3V3", j6_pins["2"][0] + g(8), j6_pins["2"][1])

    u3_sym = make_mcp23017_module()
    u3_pins = sb.place_symbol(u3_sym, "U3", "MCP23017_SSCI-016506_0x20", g(120), g(262))
    sb.add_wire(u3_pins["1"][0], u3_pins["1"][1], u3_pins["1"][0] - g(6), u3_pins["1"][1])
    sb.place_power("+3V3", u3_pins["1"][0] - g(6), u3_pins["1"][1])
    sb.add_wire(u3_pins["2"][0], u3_pins["2"][1], u3_pins["2"][0] - g(6), u3_pins["2"][1])
    sb.place_power("GND", u3_pins["2"][0] - g(6), u3_pins["2"][1])

    sw_sym = make_switch_push()
    sw5_pins = sb.place_symbol(sw_sym, "SW5", "BTN_OLED", u3_pins["5"][0] - g(14), u3_pins["5"][1],
                               ref_offset=(0, -g(3), 0), val_offset=(0, g(3), 0))
    sb.add_wire(u3_pins["5"][0], u3_pins["5"][1], sw5_pins["2"][0], sw5_pins["2"][1])
    sb.add_wire(sw5_pins["1"][0], sw5_pins["1"][1], sw5_pins["1"][0] - g(4), sw5_pins["1"][1])
    sb.place_power("GND", sw5_pins["1"][0] - g(4), sw5_pins["1"][1])

    for pnum in [str(i) for i in range(6, 13)]:
        sb.add_nc(*u3_pins[pnum])

    led_sym = make_led()
    for ch in range(1, 5):
        ax, ay = u3_pins[str(12 + ch)]
        rp = sb.place_symbol(r_sym, f"R{9+ch}", "R_TBD[G6]", ax + g(12), ay, angle=90,
                             ref_offset=(0, -g(2), 0), val_offset=(0, g(2), 0))
        sb.add_wire(ax, ay, rp["1"][0], rp["1"][1])
        dp = sb.place_symbol(led_sym, f"D{ch}", f"LED_CH{ch}", rp["2"][0] + g(12), ay, angle=180,
                             ref_offset=(0, -g(2), 0), val_offset=(0, g(2), 0))
        sb.add_wire(rp["2"][0], rp["2"][1], dp["2"][0], dp["2"][1])
        sb.add_wire(dp["1"][0], dp["1"][1], dp["1"][0] + g(4), dp["1"][1])
        sb.place_power("GND", dp["1"][0] + g(4), dp["1"][1])

    for ch in range(1, 5):
        bx, by = u3_pins[str(16 + ch)]
        swp = sb.place_symbol(sw_sym, f"SW{ch}", f"BTN_CH{ch}", bx + g(14), by,
                              ref_offset=(0, -g(2), 0), val_offset=(0, g(2), 0))
        sb.add_wire(bx, by, swp["1"][0], swp["1"][1])
        sb.add_wire(swp["2"][0], swp["2"][1], swp["2"][0] + g(4), swp["2"][1])
        sb.place_power("GND", swp["2"][0] + g(4), swp["2"][1])

    # Continuous Shared I2C Bus linking P1 #16/#17, J4 #2/#3, J1 #2/#3, J6 #3/#4, and U3 #3/#4!
    # SDA vertical trunk x = g(48); SCL vertical trunk x = g(52)
    sda_x, scl_x = g(48), g(52)
    j4_sda, j1_sda = j4_pins["2"], j1_pins["2"]
    j6_sda, u3_sda = j6_pins["4"], u3_pins["4"]
    p1_sda = p1_pins["17"]

    def build_trunk(trunk_x: float, stops_raw: List[Tuple[float, Tuple[float, float]]]) -> None:
        by_y: Dict[float, List[Tuple[float, float]]] = {}
        for y, seg in stops_raw:
            by_y.setdefault(y, []).append(seg)
        ys = sorted(by_y.keys())
        for idx, y in enumerate(ys):
            for x_a, x_b in by_y[y]:
                sb.add_wire(x_a, y, x_b, y)
            if idx > 0:
                sb.add_wire(trunk_x, ys[idx - 1], trunk_x, y)
            horiz_cnt = len(by_y[y])
            vert_cnt = (1 if idx > 0 else 0) + (1 if idx < len(ys) - 1 else 0)
            if horiz_cnt + vert_cnt >= 3:
                sb.add_junction(trunk_x, y)

    build_trunk(sda_x, [
        (j4_sda[1], (j4_sda[0], sda_x)),
        (j1_sda[1], (sda_x, j1_sda[0])),
        (g(156), (sda_x, g(138))),
        (j6_sda[1], (j6_sda[0], sda_x)),
        (u3_sda[1], (sda_x, u3_sda[0])),
    ])
    sb.add_wire_poly([(g(138), g(156)), (g(138), p1_sda[1]), (p1_sda[0], p1_sda[1])])
    sb.add_label("I2C_SDA", g(76), g(156), angle=0)

    j4_scl, j1_scl = j4_pins["3"], j1_pins["3"]
    j6_scl, u3_scl = j6_pins["3"], u3_pins["3"]
    p1_scl = p1_pins["16"]
    build_trunk(scl_x, [
        (j4_scl[1], (j4_scl[0], scl_x)),
        (j1_scl[1], (scl_x, j1_scl[0])),
        (g(152), (scl_x, g(135))),
        (j6_scl[1], (j6_scl[0], scl_x)),
        (u3_scl[1], (scl_x, u3_scl[0])),
    ])
    sb.add_wire_poly([(g(135), g(152)), (g(135), p1_scl[1]), (p1_scl[0], p1_scl[1])])
    sb.add_label("I2C_SCL", g(76), g(152), angle=0)

    sb.add_text(
        "PCB has R44/R46 2.2k I2C PU -> +3V3. SSCI-016506 /RESET fixed internally (polling only; NO ATX/KVM!).",
        g(15), g(310), size=1.1, color=(120, 50, 0, 1.0)
    )

    # ==================================================================
    # ZONE 4: ATX PWR/RST outputs (A,B + ~E -> 74HC139 x1 -> 8x Photorelay) & KVM DIRECT GPIO I/O
    # Owner sketch 2026-11-28 / Issue #180: the KVM button (open-drain, 3.3V pull-up, press=0V) and the
    # KVM LED (3.3V common, KVM IC sinks, lit=0V) connect to GPIO directly; no isolation parts.
    # ==================================================================
    dec_sym = make_74hc139()
    pr_sym = make_photorelay()

    # P1 #35 (A0), #34 (A1) -> vertical A/B trunks (x = g(244), g(250)) feeding U1 unit 1 and unit 2.
    # Fail-safe 100k PD resistors R3, R4 keep channel 1 selected at reset (all ~E_* are High anyway).
    a0_x, a1_x = g(244), g(250)
    u1_u1 = sb.place_symbol(dec_sym, "U1", "74HC139", g(276), g(46), unit=1)
    u1_u2 = sb.place_symbol(dec_sym, "U1", "74HC139", g(276), g(108), unit=2)

    r3_pins = sb.place_symbol(r_sym, "R3", "100k_PD", g(228), g(32), angle=0,
                              ref_offset=(g(3), -g(1), 0), val_offset=(g(3), g(1), 0))
    sb.add_wire(r3_pins["2"][0], r3_pins["2"][1], r3_pins["2"][0], r3_pins["2"][1] + g(2))
    sb.place_power("GND", r3_pins["2"][0], r3_pins["2"][1] + g(2))

    r4_pins = sb.place_symbol(r_sym, "R4", "100k_PD", g(224), g(50), angle=0,
                              ref_offset=(-g(4), -g(1), 0), val_offset=(-g(4), g(1), 0))
    sb.add_wire(r4_pins["2"][0], r4_pins["2"][1], r4_pins["2"][0], r4_pins["2"][1] + g(2))
    sb.place_power("GND", r4_pins["2"][0], r4_pins["2"][1] + g(2))

    # A0 trunk (a0_x): R3 top, P1 #35 -> elbow, U1 unit 1 pin 2 (1A), U1 unit 2 pin 14 (2A)
    p35x, p35y = p1_pins["35"]
    sb.add_wire_poly([(p35x, p35y), (g(218), p35y), (g(218), r3_pins["1"][1] - g(2)), (r3_pins["1"][0], r3_pins["1"][1] - g(2)), (a0_x, r3_pins["1"][1] - g(2))])
    sb.add_wire(r3_pins["1"][0], r3_pins["1"][1], r3_pins["1"][0], r3_pins["1"][1] - g(2))
    sb.add_junction(r3_pins["1"][0], r3_pins["1"][1] - g(2))
    sb.add_label("KVM_A0", g(220), r3_pins["1"][1] - g(2), angle=0)

    a0_ys = [r3_pins["1"][1] - g(2), u1_u1["2"][1], u1_u2["14"][1]]
    a0_pins_x = [None, u1_u1["2"][0], u1_u2["14"][0]]
    for i, y in enumerate(a0_ys):
        if a0_pins_x[i] is not None:
            sb.add_wire(a0_x, y, a0_pins_x[i], y)
        if i > 0:
            sb.add_wire(a0_x, a0_ys[i - 1], a0_x, y)
        if 0 < i < len(a0_ys) - 1:
            sb.add_junction(a0_x, y)

    # A1 trunk (a1_x): P1 #34 -> elbow, R4 top, U1 unit 1 pin 3 (1B), U1 unit 2 pin 13 (2B)
    p34x, p34y = p1_pins["34"]
    a1_top_y = u1_u1["3"][1]
    sb.add_wire_poly([(p34x, p34y), (g(221), p34y), (g(221), a1_top_y), (r4_pins["1"][0], a1_top_y), (a1_x, a1_top_y), (u1_u1["3"][0], a1_top_y)])
    sb.add_wire(r4_pins["1"][0], r4_pins["1"][1], r4_pins["1"][0], a1_top_y)
    sb.add_junction(r4_pins["1"][0], a1_top_y)
    sb.add_junction(a1_x, a1_top_y)
    sb.add_label("KVM_A1", g(226), a1_top_y, angle=0)
    sb.add_wire(a1_x, a1_top_y, a1_x, u1_u2["13"][1])
    sb.add_wire(a1_x, u1_u2["13"][1], u1_u2["13"][0], u1_u2["13"][1])

    # ~{E_PWR} (P1 #32), ~{E_RST} (P1 #31) with 10k PU (R5, R6) directly into the 74HC139 ~G pins
    en_routes = [
        ("32", "~{E_PWR}", u1_u1["1"], "R5", g(230), g(256)),
        ("31", "~{E_RST}", u1_u2["15"], "R6", g(233), g(258)),
    ]
    for pnum, lname, gpin, rref, tap_x, pu_x in en_routes:
        px, py = p1_pins[pnum]
        gx, gy = gpin
        sb.add_wire_poly([(px, py), (tap_x, py), (tap_x, gy), (pu_x, gy)])
        sb.add_wire(pu_x, gy, gx, gy)
        rp = sb.place_symbol(r_sym, rref, "10k_PU", pu_x, gy - g(6), angle=0,
                             ref_offset=(g(3), -g(1), 0), val_offset=(g(3), g(1), 0))
        sb.add_wire(pu_x, gy, rp["2"][0], rp["2"][1])
        sb.add_junction(pu_x, gy)
        sb.add_wire(rp["1"][0], rp["1"][1], rp["1"][0], rp["1"][1] - g(2))
        sb.place_power("+3V3", rp["1"][0], rp["1"][1] - g(2))
        sb.add_label(lname, g(238), gy, angle=0)

    # 2 banks of 4 photorelays (PWR SW, RST SW) wired directly to the 74HC139 outputs.
    # Relay input LED: +3V3 -> R (390 ohm, owner sketch) -> LED -> decoder output (Low = ON).
    j2p_sym = make_conn_1xN_left(
        "quad_ipkvm:Conn_Out_2P", 2, ["OUT_A", "OUT_B"], width_units=8, spacing_units=4,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"
    )
    bank_specs = [
        ("ATX POWER SW (CH1..4)", u1_u1, ["4", "5", "6", "7"], "PWR_DRV_CH", 1, "J7", "PWR", g(28)),
        ("ATX RESET SW (CH1..4)", u1_u2, ["12", "11", "10", "9"], "RST_DRV_CH", 5, "J8", "RST", g(90)),
    ]
    for title_txt, dec_pins, out_pnums, drv_prefix, k_start, j_ref, tag, y_top in bank_specs:
        sb.add_text(title_txt, g(330), y_top - g(5), size=1.35, bold=True)
        for ch_idx, out_p in enumerate(out_pnums):
            ch = ch_idx + 1
            k_num = k_start + ch_idx
            ky = y_top + ch_idx * g(14)
            kx = g(384)
            kp = sb.place_symbol(pr_sym, f"K{k_num}", "G3VM-61A1[C1]", kx, ky,
                                 ref_offset=(0, -g(5), 0), val_offset=(0, g(5), 0))
            ax, ay = kp["1"]
            rp = sb.place_symbol(r_sym, f"R{20+k_num}", "390[C3]", ax - g(8), ay, angle=90,
                                 ref_offset=(0, -g(4), 0), val_offset=(0, -g(2), 0))
            sb.add_wire(rp["2"][0], rp["2"][1], ax, ay)
            sb.add_wire(rp["1"][0], rp["1"][1], rp["1"][0] - g(4), rp["1"][1])
            sb.place_power("+3V3", rp["1"][0] - g(4), rp["1"][1])

            dx, dy = dec_pins[out_p]
            cat_x, cat_y = kp["2"]
            step_x = g(302 + ch_idx * 6)
            sb.add_wire_poly([(dx, dy), (step_x, dy), (step_x, cat_y), (cat_x, cat_y)])
            sb.add_label(f"{drv_prefix}{ch}", g(334), cat_y, angle=0)

            jp = sb.place_symbol(j2p_sym, f"{j_ref}_{ch}", f"{tag}_CH{ch}", kx + g(20), ky,
                                 ref_offset=(g(12), -g(2), 0), val_offset=(g(12), g(2), 0))
            sb.add_wire(kp["3"][0], kp["3"][1], jp["1"][0], jp["1"][1])
            sb.add_wire(kp["4"][0], kp["4"][1], jp["2"][0], jp["2"][1])

    # --- KVM DIRECT GPIO I/O (owner answers A2..A8, 2026-11-28): 9-pin harness to the 4ch HDMI KVM ---
    sb.add_text("KVM DIRECT GPIO I/O (3.3V logic, NO isolation) - Issue #180", g(330), g(147), size=1.35, bold=True)
    j_kvm_sym = make_conn_1xN_left(
        "quad_ipkvm:KVM_Direct_9P", 9,
        ["BTN1", "BTN2", "BTN3", "BTN4", "LED1", "LED2", "LED3", "LED4", "GND"],
        width_units=8, spacing_units=4,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x09_P2.54mm_Vertical",
    )
    jk = sb.place_symbol(j_kvm_sym, "J9", "KVM_DIRECT_9P", g(396), g(170),
                         ref_offset=(g(8), -g(20), 0), val_offset=(g(8), g(20), 0))
    gx9, gy9 = jk["9"]
    sb.add_wire(gx9, gy9, gx9 - g(4), gy9)
    sb.place_power("GND", gx9 - g(4), gy9)
    for pnum, lname in [("1", "KVM_BTN1"), ("2", "KVM_BTN2"), ("3", "KVM_BTN3"), ("4", "KVM_BTN4"),
                        ("5", "KVM_LED1"), ("6", "KVM_LED2"), ("7", "KVM_LED3"), ("8", "KVM_LED4")]:
        kx9, ky9 = jk[pnum]
        sb.add_wire(kx9, ky9, kx9 - g(4), ky9)
        sb.add_label(lname, kx9 - g(4), ky9, angle=180)
    sb.add_text(
        "BTN1..4: 3.3V pull-up in the KVM, press = 0V\n"
        "  [A3,A4,A5] -> ESP OPEN-DRAIN:\n"
        "  Low = press, Hi-Z = release. NEVER High.\n"
        "LED1..4: 3.3V common, KVM IC sinks\n"
        "  lit = 0V (Active Low) [A6,A7],\n"
        "  no auto-scan [A8] -> GPIO input.\n"
        "TODO(hw-verify): LED off-level / pull-up,\n"
        "  reverse-feed (KVM on, ESP off), GND.\n"
        "kvm_board driver has ONE e_kvm output:\n"
        "  it cannot drive 4 open-drain buttons.\n"
        "  Do NOT enable it with this harness (A-9).",
        g(330), g(151), size=1.05, color=(40, 20, 20, 1.0)
    )

    # ==================================================================
    # ZONE 5: PC front-panel PWR LED / HDD LED sense (owner sketch 2026-11-28)
    # Per channel: PC LED+ -> 390 ohm -> photorelay input LED; output 3.3V -> contact -> node -> 4.7k to GND.
    # 74HC4052 (U2) selects the channel with the SAME A/B as the 74HC139 (KVM_A0/KVM_A1) and returns
    # PC_PWR_L / PC_HDD_L to two GPIO inputs.
    # ==================================================================
    mux_sym = make_74hc4052()
    prm_sym = make_photorelay_mirror()
    mx, my = g(288), g(264)
    mux = sb.place_symbol(mux_sym, "U2", "74HC4052", mx, my,
                          ref_offset=(g(10), g(36), 0), val_offset=(g(10), g(39), 0))

    # top pins: selects + commons as stub labels (left-pointing, staggered heights), VCC via +3V3
    for pnum, lname, h in [("10", "KVM_A0", 4), ("9", "KVM_A1", 7), ("13", "PC_PWR_L", 10), ("3", "PC_HDD_L", 13)]:
        tx, ty = mux[pnum]
        sb.add_wire(tx, ty, tx, ty - g(h))
        sb.add_label(lname, tx, ty - g(h), angle=180)
    vx, vy = mux["16"]
    sb.add_wire(vx, vy, vx, vy - g(2))
    sb.place_power("+3V3", vx, vy - g(2))
    # bottom pins: ~E, VEE, GND all tied to GND (single 3.3V supply, always enabled)
    ex, ey = mux["6"]
    ve_x, ve_y = mux["7"]
    gn_x, gn_y = mux["8"]
    bus_y = ey + g(3)
    sb.add_wire(ex, ey, ex, bus_y)
    sb.add_wire(ve_x, ve_y, ve_x, bus_y)
    sb.add_wire(gn_x, gn_y, gn_x, bus_y)
    sb.add_wire(ex, bus_y, ve_x, bus_y)
    sb.add_wire(ve_x, bus_y, gn_x, bus_y)
    sb.add_junction(ve_x, bus_y)
    sb.place_power("GND", ve_x, bus_y)
    # decoupling for U2
    c_sym2 = make_cap()
    cp2 = sb.place_symbol(c_sym2, "C2", "100nF", g(304), g(226))
    sb.add_wire(cp2["1"][0], cp2["1"][1], cp2["1"][0], cp2["1"][1] - g(2))
    sb.place_power("+3V3", cp2["1"][0], cp2["1"][1] - g(2))
    sb.add_wire(cp2["2"][0], cp2["2"][1], cp2["2"][0], cp2["2"][1] + g(2))
    sb.place_power("GND", cp2["2"][0], cp2["2"][1] + g(2))

    sb.add_text("PWR LED sense (CH1..4)", g(222), g(224), size=1.25, bold=True)
    sb.add_text("HDD LED sense (CH1..4)", g(318), g(224), size=1.25, bold=True)

    pc_p_conn = make_conn_1xN(
        "quad_ipkvm:PC_PWR_LED_2P", 2, ["PWR_LED+", "PWR_LED-"], width_units=8, spacing_units=4,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"
    )
    pc_h_conn = make_conn_1xN_left(
        "quad_ipkvm:PC_HDD_LED_2P", 2, ["HDD_LED+", "HDD_LED-"], width_units=8, spacing_units=4,
        footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"
    )
    ch_left = ["12", "14", "15", "11"]    # 1Y0..1Y3 -> PWR LED CH1..4
    ch_right = ["1", "5", "2", "4"]       # 2Y0..2Y3 -> HDD LED CH1..4
    row_ys = [my - g(24), my - g(8), my + g(8), my + g(24)]
    for idx in range(4):
        ch = idx + 1
        ry = row_ys[idx]
        ky = ry - g(2)  # relay centre: S2 (lower output pin) lands exactly on the row y

        # ---------------- PWR LED block (left of the mux) ----------------
        kpp = sb.place_symbol(pr_sym, f"K{8 + ch}", "G3VM-61A1[C1]", g(255), ky,
                              ref_offset=(0, -g(5), 0), val_offset=(0, g(5), 0))
        jpp = sb.place_symbol(pc_p_conn, f"J10_{ch}", f"PC_PWR_LED_CH{ch}", g(230), ky)
        rpp = sb.place_symbol(r_sym, f"R{40 + ch}", "390[C3]", g(244), kpp["1"][1], angle=90,
                              ref_offset=(0, -g(4), 0), val_offset=(0, -g(2), 0))
        sb.add_wire(jpp["1"][0], jpp["1"][1], rpp["1"][0], rpp["1"][1])
        sb.add_wire(rpp["2"][0], rpp["2"][1], kpp["1"][0], kpp["1"][1])
        sb.add_wire(kpp["2"][0], kpp["2"][1], jpp["2"][0], jpp["2"][1])
        sb.add_wire(kpp["3"][0], kpp["3"][1], kpp["3"][0], kpp["3"][1] - g(2))
        sb.place_power("+3V3", kpp["3"][0], kpp["3"][1] - g(2))
        nx = g(265)
        sb.add_wire(kpp["4"][0], kpp["4"][1], nx, ry)
        sb.add_junction(nx, ry)
        sb.add_wire(nx, ry, mux[ch_left[idx]][0], ry)
        rdp = sb.place_symbol(r_sym, f"R{48 + ch}", "4.7k[C3]", nx, ry + g(7), angle=0,
                              ref_offset=(g(5), -g(1), 0), val_offset=(g(5), g(1), 0))
        sb.add_wire(nx, ry, rdp["1"][0], rdp["1"][1])
        sb.add_wire(rdp["2"][0], rdp["2"][1], rdp["2"][0], rdp["2"][1] + g(2))
        sb.place_power("GND", rdp["2"][0], rdp["2"][1] + g(2))

        # ---------------- HDD LED block (right of the mux, mirrored) ----------------
        kph = sb.place_symbol(prm_sym, f"K{12 + ch}", "G3VM-61A1[C1]", g(321), ky,
                              ref_offset=(0, -g(5), 0), val_offset=(0, g(5), 0))
        jph = sb.place_symbol(pc_h_conn, f"J11_{ch}", f"PC_HDD_LED_CH{ch}", g(346), ky)
        rph = sb.place_symbol(r_sym, f"R{44 + ch}", "390[C3]", g(332), kph["1"][1], angle=90,
                              ref_offset=(0, -g(4), 0), val_offset=(0, -g(2), 0))
        sb.add_wire(kph["1"][0], kph["1"][1], rph["1"][0], rph["1"][1])
        sb.add_wire(rph["2"][0], rph["2"][1], jph["1"][0], jph["1"][1])
        sb.add_wire(kph["2"][0], kph["2"][1], jph["2"][0], jph["2"][1])
        sb.add_wire(kph["3"][0], kph["3"][1], kph["3"][0], kph["3"][1] - g(2))
        sb.place_power("+3V3", kph["3"][0], kph["3"][1] - g(2))
        nxh = g(311)
        sb.add_wire(kph["4"][0], kph["4"][1], nxh, ry)
        sb.add_junction(nxh, ry)
        sb.add_wire(nxh, ry, mux[ch_right[idx]][0], ry)
        rdh = sb.place_symbol(r_sym, f"R{52 + ch}", "4.7k[C3]", nxh, ry + g(7), angle=0,
                              ref_offset=(-g(5), -g(1), 0), val_offset=(-g(5), g(1), 0))
        sb.add_wire(nxh, ry, rdh["1"][0], rdh["1"][1])
        sb.add_wire(rdh["2"][0], rdh["2"][1], rdh["2"][0], rdh["2"][1] + g(2))
        sb.place_power("GND", rdh["2"][0], rdh["2"][1] + g(2))

    sb.add_text(
        "74HC4052: VEE = ~E = GND (always enabled), A/B = KVM_A0/KVM_A1 shared with the 74HC139.\n"
        "PC front panel: LED+ -> 390 ohm -> relay input LED; output 3.3V -> contact -> 4.7k to GND.\n"
        "TODO(hw-verify): 4052 pinout/Ron@3.3V (datasheet), relay If/Vf, PC header polarity/voltage [C1,C4].",
        g(222), g(309), size=1.05, color=(40, 20, 20, 1.0)
    )

    # ==================================================================
    # BOTTOM-RIGHT AREA: 74HC139 power & decoupling, notes
    # ==================================================================
    sb.add_rect(g(366), g(208), g(456), g(244), width=0.254, stroke_type="dash", color=(110, 40, 160, 1.0))
    sb.add_text("74HC139 POWER & DECOUPLING (U1 power unit)", g(369), g(210), size=1.35, bold=True, color=(90, 30, 140, 1.0))

    c_sym = make_cap()
    ux, uy = g(384), g(228)
    up = sb.place_symbol(dec_sym, "U1", "74HC139", ux, uy, unit=3,
                         ref_offset=(-g(6), -g(1), 0), val_offset=(-g(6), g(2), 0))
    sb.add_wire(up["16"][0], up["16"][1], up["16"][0], up["16"][1] - g(2))
    sb.place_power("+3V3", up["16"][0], up["16"][1] - g(2))
    sb.add_wire(up["8"][0], up["8"][1], up["8"][0], up["8"][1] + g(2))
    sb.place_power("GND", up["8"][0], up["8"][1] + g(2))
    cp = sb.place_symbol(c_sym, "C1", "100nF", ux + g(11), uy)
    sb.add_wire(cp["1"][0], cp["1"][1], cp["1"][0], cp["1"][1] - g(2))
    sb.place_power("+3V3", cp["1"][0], cp["1"][1] - g(2))
    sb.add_wire(cp["2"][0], cp["2"][1], cp["2"][0], cp["2"][1] + g(2))
    sb.place_power("GND", cp["2"][0], cp["2"][1] + g(2))

    # Notes & Safety Box above A2 Title Block (x = g(366)..g(456), y = g(248)..g(288))
    sb.add_rect(g(366), g(248), g(456), g(289), width=0.254, color=(160, 20, 20, 1.0))
    sb.add_text(
        "SAFETY RULES, PCB-FIXED I/F & TODO(hw-verify) NOTES\n"
        "- [H3] NEVER connect J3 #1 (VCC_5V) to PC VBUS. NEVER connect X630 #2 (HDET=4.8V).\n"
        "- [PLAN 4.4] ATX/KVM ctrl & LED sense MUST use direct GPIO (NEVER MCP23017).\n"
        "- Invariant 4: Keep both ~E_* High when changing A0/A1 (R3/R4 100k PD, R5/R6 10k PU).\n"
        "- KVM: button = ESP OPEN-DRAIN (Low=press, never High); LED = Active Low (lit=0V) [A3-A8, Issue #180].\n"
        "  Reverse-feed (KVM on/ESP off) is UNRESOLVED: pick one before connecting the harness (Issue #180):\n"
        "  KVM pull-up only while ESP I/O is on / no pull-up path to the pin / series R + abs-max check [GPIO abs-max: not in repo docs].\n"
        "- Relay LED (390 ohm): IF must reach the datasheet recommended value at min VCC, driver VOL, R tolerance, temp [C1/C3].\n"
        "- PROVISIONAL GPIOs: A0/A1 P1#35/#34, ~E_PWR/~E_RST #32/#31, KVM_BTN1 #24, KVM_LED1..4 #26/#25/#22/#21,\n"
        "  KVM_BTN2..4 #2/#7/#9 (GPIO19/15/14), PC_PWR_L #10 (GPIO6), PC_HDD_L #1 (GPIO54). No spare GPIO left.\n"
        "- PCB-Fixed (internal): H2 UART (G37/38), J4 100M ETH (G28-31,34,35,49-52), TF1 SD (G39-45),\n"
        "  U9/U11 Audio (G9-13,53 + shared I2C; check ES8311 addr [G2]), Strapping (G34-38).",
        g(368), g(250), size=1.12, color=(40, 20, 20, 1.0)
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


def render_project_sym_lib(lib_defs: Dict[str, LibSymDef]) -> str:
    """Emit quad_ipkvm.kicad_sym containing all custom quad_ipkvm:* symbols."""
    custom = [lib_defs[k].to_sexpr(indent="  ", use_short_name=True)
              for k in sorted(lib_defs) if k.startswith("quad_ipkvm:")]
    body = "\n".join(custom)
    return (
        f'(kicad_symbol_lib\n'
        f'  (version {KICAD_VERSION})\n'
        f'  (generator {q(GENERATOR_NAME)})\n'
        f'  (generator_version "1.0")\n'
        f'{body}\n'
        f')\n'
    )


def render_sym_lib_table() -> str:
    return (
        '(sym_lib_table\n'
        '  (version 7)\n'
        '  (lib (name "quad_ipkvm")(type "KiCad")(uri "${KIPRJMOD}/quad_ipkvm.kicad_sym")(options "")(descr "quad-ipkvm project symbols"))\n'
        ')\n'
    )


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else repo_root / "hardware" / "kicad"
    out_dir.mkdir(parents=True, exist_ok=True)

    root_uuid = det_uuid("sheet:root")
    sb = build_unified_sheet()

    pro_path = out_dir / "quad-ipkvm.kicad_pro"
    sch_path = out_dir / "quad-ipkvm.kicad_sch"
    sym_path = out_dir / "quad_ipkvm.kicad_sym"
    tbl_path = out_dir / "sym-lib-table"

    pro_path.write_text(json.dumps(build_project_json(root_uuid), indent=2) + "\n", encoding="utf-8")
    sch_path.write_text(sb.render_sch(), encoding="utf-8")
    sym_path.write_text(render_project_sym_lib(sb.lib_defs), encoding="utf-8")
    tbl_path.write_text(render_sym_lib_table(), encoding="utf-8")

    print(f"Generated {sch_path} (A2 unified single sheet, {len(sb.items)} items, {len(sb.lib_defs)} lib_symbols)")
    print(f"Generated {sym_path}, {tbl_path}, and {pro_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
