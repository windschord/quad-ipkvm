#!/usr/bin/env python3
"""Verify that kvm_board GPTimer ISR symbols live in IRAM and contain no flash calls or refs.

Inspects a compiled ESP32-P4 ELF binary using riscv32-esp-elf-objdump:
  1. Confirms .iram* and .flash* sections exist (catches wrong/stripped ELF).
  2. Confirms `on_timer_alarm`, `kvm_board_logic_on_timer_alarm`, and (if emitted
     out-of-line) `line_to_gpio*` reside in an `.iram*` section.
  3. Recursively inspects `on_timer_alarm` and its `kvm_board`-owned callees in
     disassembly and fails on:
       - Any branch/jump/call operand or `# <addr>` PC-relative resolution landing
         in any `.flash*` section (catches flash code and `.flash.rodata`).
       - Any symbol in `.flash*` referenced in instruction annotations.
       - Any unannotated indirect jump/call (`jalr` / `c.jalr` without `# <addr>`).
"""
from __future__ import annotations

import argparse
import bisect
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Set, Tuple


REQUIRED_IRAM_ROOTS = ("on_timer_alarm", "kvm_board_logic_on_timer_alarm")
OPTIONAL_IRAM_PREFIXES = ("line_to_gpio", "isr_gpio_set_high", "kvm_board_drv_eval_alarm")
# External IDF/FreeRTOS IRAM helpers invoked by portENTER/EXIT_CRITICAL_ISR whose
# assert()-only failure branches reference __assert_func strings in .flash.rodata.
EXTERNAL_CRITICAL_HELPERS = (
    "xPortEnterCriticalTimeout",
    "vPortExitCriticalMultiCore",
)

INSN_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+(\S+)(?:\s+(.*))?$"
)
TARGET_HEX_RE = re.compile(r"\b([0-9a-fA-F]{6,8})\s+<([^>]+)>")
COMMENT_ADDR_RE = re.compile(r"#\s*([0-9a-fA-F]{6,8})(?:\s+<([^>]+)>)?")
BRANCH_MNEMONICS = {
    "j",
    "jal",
    "c.j",
    "c.jal",
    "beqz",
    "bnez",
    "beq",
    "bne",
    "blt",
    "bge",
    "bltu",
    "bgeu",
    "blez",
    "bgez",
    "bltz",
    "bgtz",
}
INDIRECT_MNEMONICS = {"jalr", "jr", "c.jalr", "c.jr"}


@dataclass(frozen=True)
class Section:
    name: str
    start: int
    size: int

    @property
    def end(self) -> int:
        return self.start + self.size


@dataclass(frozen=True)
class Symbol:
    name: str
    section: str
    addr: int
    size: int


def run_objdump(objdump: str, args: Sequence[str], elf: str) -> str:
    try:
        res = subprocess.run(
            [objdump, *args, elf],
            capture_output=True,
            text=True,
            check=True,
        )
        return res.stdout
    except FileNotFoundError as exc:
        raise RuntimeError(f"objdump binary not found: {objdump}") from exc
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"objdump {' '.join(args)} failed (rc={exc.returncode}): {exc.stderr.strip()}"
        ) from exc


def parse_sections(text: str) -> Tuple[List[Section], List[Section], Dict[str, Section]]:
    iram: List[Section] = []
    flash: List[Section] = []
    by_name: Dict[str, Section] = {}
    sec_re = re.compile(
        r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+"
    )
    for line in text.splitlines():
        m = sec_re.match(line)
        if not m:
            continue
        name = m.group(1)
        size = int(m.group(2), 16)
        vma = int(m.group(3), 16)
        if size == 0 or vma == 0:
            continue
        sec = Section(name=name, start=vma, size=size)
        by_name[name] = sec
        if name.startswith(".iram"):
            iram.append(sec)
        elif "flash" in name:
            flash.append(sec)
    iram.sort(key=lambda s: s.start)
    flash.sort(key=lambda s: s.start)
    return iram, flash, by_name


def parse_symbols(text: str) -> Dict[int, List[Symbol]]:
    by_addr: Dict[int, List[Symbol]] = {}
    # Format: <addr> <flags(7 chars or cols)> <section> <size/align> <name>
    sym_re = re.compile(
        r"^([0-9a-fA-F]+)\s+\S+\s+(?:[DFOdf]\s+)?(\S+)\s+([0-9a-fA-F]+)\s+(\S+)\s*$"
    )
    for line in text.splitlines():
        m = sym_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        sec = m.group(2)
        size = int(m.group(3), 16)
        name = m.group(4)
        sym = Symbol(name=name, section=sec, addr=addr, size=size)
        by_addr.setdefault(addr, []).append(sym)
    return by_addr


def addr_in_sections(addr: int, sections: Sequence[Section]) -> Optional[Section]:
    for s in sections:
        if s.start <= addr < s.end:
            return s
    return None


def find_symbols_by_name(
    by_addr: Dict[int, List[Symbol]], name: str, prefix_match: bool = False
) -> List[Symbol]:
    out: List[Symbol] = []
    for syms in by_addr.values():
        for s in syms:
            if s.section == "*ABS*" and s.addr == 0:
                continue
            if prefix_match:
                if s.name == name or s.name.startswith(name + "."):
                    out.append(s)
            elif s.name == name:
                out.append(s)
    return out


def find_enclosing_function(
    sorted_funcs: Sequence[Tuple[int, Symbol]], addr: int
) -> Optional[Symbol]:
    starts = [a for a, _ in sorted_funcs]
    idx = bisect.bisect_right(starts, addr) - 1
    if idx < 0:
        return None
    f_addr, sym = sorted_funcs[idx]
    if f_addr <= addr < f_addr + max(sym.size, 4):
        return sym
    return None


def verify_function(
    objdump: str,
    elf: str,
    root_sym: Symbol,
    flash_sections: Sequence[Section],
    iram_sections: Sequence[Section],
    sorted_funcs: Sequence[Tuple[int, Symbol]],
    visited: Set[int],
    errors: List[str],
    checked: List[str],
) -> None:
    if root_sym.addr in visited:
        return
    visited.add(root_sym.addr)
    checked.append(f"{root_sym.name}@{hex(root_sym.addr)} ({root_sym.section}, {root_sym.size}B)")

    if root_sym.size == 0:
        errors.append(f"Symbol {root_sym.name} has zero size")
        return

    dis = run_objdump(
        objdump,
        [
            "-d",
            f"--start-address={hex(root_sym.addr)}",
            f"--stop-address={hex(root_sym.addr + root_sym.size)}",
        ],
        elf,
    )

    recurse_targets: List[Symbol] = []
    for line in dis.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        pc = int(m.group(1), 16)
        mnem = m.group(2)
        rest = m.group(3) or ""

        # Check all `# <hex> <sym>` address annotations (covers auipc+addi rodata & auipc+jalr call).
        cmt = COMMENT_ADDR_RE.search(rest)
        if cmt:
            ref_addr = int(cmt.group(1), 16)
            ref_name = cmt.group(2) or ""
            flash_sec = addr_in_sections(ref_addr, flash_sections)
            if flash_sec is not None:
                errors.append(
                    f"{root_sym.name}+0x{pc - root_sym.addr:x} ({mnem}): "
                    f"references flash section {flash_sec.name} at {hex(ref_addr)} <{ref_name}>"
                )
            elif (
                mnem in INDIRECT_MNEMONICS
                and ref_name
                and ref_name.split("+")[0] not in EXTERNAL_CRITICAL_HELPERS
            ):
                target = find_enclosing_function(sorted_funcs, ref_addr)
                if target and target.addr not in visited:
                    recurse_targets.append(target)
        elif mnem in INDIRECT_MNEMONICS:
            # Allow function returns (`jr ra` / `c.jr ra`) and tail/switch? Ban indirect calls.
            operand = rest.split("#", 1)[0].strip()
            if mnem in {"jalr", "c.jalr"} or operand not in {"ra"}:
                errors.append(
                    f"{root_sym.name}+0x{pc - root_sym.addr:x} ({mnem} {operand}): "
                    f"unannotated indirect branch/call in IRAM ISR path"
                )

        # Check explicit branch/jump target `<addr> <sym>`.
        for tgt_addr_s, tgt_name in TARGET_HEX_RE.findall(rest.split("#", 1)[0]):
            tgt_addr = int(tgt_addr_s, 16)
            flash_sec = addr_in_sections(tgt_addr, flash_sections)
            if flash_sec is not None:
                errors.append(
                    f"{root_sym.name}+0x{pc - root_sym.addr:x} ({mnem}): "
                    f"jumps/calls into flash {flash_sec.name} at {hex(tgt_addr)} <{tgt_name}>"
                )
            elif mnem in BRANCH_MNEMONICS:
                base_name = tgt_name.split("+")[0]
                if base_name == root_sym.name or base_name in EXTERNAL_CRITICAL_HELPERS:
                    continue
                target = find_enclosing_function(sorted_funcs, tgt_addr)
                if target and target.addr not in visited:
                    # Ensure target itself is in IRAM if it's a code symbol (not *ABS* ROM).
                    if target.section != "*ABS*" and not target.section.startswith(".iram"):
                        errors.append(
                            f"{root_sym.name}+0x{pc - root_sym.addr:x} calls {target.name} "
                            f"in non-IRAM section {target.section}"
                        )
                    elif addr_in_sections(target.addr, iram_sections) is not None:
                        recurse_targets.append(target)

    for target in recurse_targets:
        verify_function(
            objdump,
            elf,
            target,
            flash_sections,
            iram_sections,
            sorted_funcs,
            visited,
            errors,
            checked,
        )


def check_elf(elf_path: str, objdump: str) -> int:
    sec_out = run_objdump(objdump, ["-h"], elf_path)
    iram_sections, flash_sections, _ = parse_sections(sec_out)
    if not iram_sections:
        print("ERROR: no non-empty .iram* sections found in ELF", file=sys.stderr)
        return 1
    if not flash_sections:
        print("ERROR: no non-empty .flash* sections found in ELF", file=sys.stderr)
        return 1

    sym_out = run_objdump(objdump, ["-t"], elf_path)
    by_addr = parse_symbols(sym_out)

    sorted_funcs: List[Tuple[int, Symbol]] = []
    for addr, syms in sorted(by_addr.items()):
        for s in syms:
            if s.size > 0 and s.section != "*ABS*":
                sorted_funcs.append((addr, s))

    errors: List[str] = []
    roots_to_check: List[Symbol] = []

    for req_name in REQUIRED_IRAM_ROOTS:
        matches = find_symbols_by_name(by_addr, req_name, prefix_match=False)
        if not matches:
            errors.append(f"Missing required symbol `{req_name}` (is CONFIG_KVM_BOARD_ENABLE=y?)")
            continue
        for sym in matches:
            if not sym.section.startswith(".iram"):
                errors.append(
                    f"Symbol `{sym.name}` at {hex(sym.addr)} is in `{sym.section}`, expected `.iram*`"
                )
            else:
                roots_to_check.append(sym)

    for opt_prefix in OPTIONAL_IRAM_PREFIXES:
        for sym in find_symbols_by_name(by_addr, opt_prefix, prefix_match=True):
            if not sym.section.startswith(".iram"):
                errors.append(
                    f"Emitted helper `{sym.name}` at {hex(sym.addr)} is in `{sym.section}`, expected `.iram*`"
                )
            else:
                roots_to_check.append(sym)

    # Also ensure gpio_ll_set_level was never outlined into flash.
    for sym in find_symbols_by_name(by_addr, "gpio_ll_set_level", prefix_match=True):
        if "flash" in sym.section or not sym.section.startswith(".iram"):
            errors.append(
                f"`{sym.name}` was outlined into `{sym.section}` at {hex(sym.addr)}"
            )

    visited: Set[int] = set()
    checked: List[str] = []
    for sym in roots_to_check:
        verify_function(
            objdump,
            elf_path,
            sym,
            flash_sections,
            iram_sections,
            sorted_funcs,
            visited,
            errors,
            checked,
        )

    if errors:
        print("kvm_board IRAM check FAILED:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print("kvm_board IRAM check PASSED:")
    for item in checked:
        print(f"  - {item}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", help="Path to quad-ipkvm.elf")
    parser.add_argument(
        "--objdump",
        default="riscv32-esp-elf-objdump",
        help="Path or name of RISC-V objdump executable (default: riscv32-esp-elf-objdump)",
    )
    args = parser.parse_args()
    try:
        return check_elf(args.elf, args.objdump)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
