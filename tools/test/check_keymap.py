#!/usr/bin/env python3
"""Checks the KeyboardEvent.code -> HID Usage table in components/web_server/web/app.js (Keyboard/Keypad Page 0x07).

The expected table below is written from the USB HID Usage Tables (Keyboard/Keypad page) and the UI Events
KeyboardEvent.code values, NOT copied from app.js. The test also runs against mutated copies of app.js
(`--mutations`): every mutant must be detected.

Usage: tools/test/check_keymap.py [--mutations]      (standard library only; run from anywhere)
Exit status: 0 = ok, 1 = a check failed (or a mutant survived)
"""
import os
import re
import sys

APP_JS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'components', 'web_server', 'web', 'app.js')

EXPECTED = {}
for i, c in enumerate('ABCDEFGHIJKLMNOPQRSTUVWXYZ'):
    EXPECTED['Key' + c] = 0x04 + i
for i, c in enumerate('1234567890'):
    EXPECTED['Digit' + c] = 0x1E + i
EXPECTED.update({
    'Enter': 0x28, 'Escape': 0x29, 'Backspace': 0x2A, 'Tab': 0x2B, 'Space': 0x2C, 'Minus': 0x2D, 'Equal': 0x2E,
    'BracketLeft': 0x2F, 'BracketRight': 0x30, 'Backslash': 0x31, 'IntlHash': 0x32, 'Semicolon': 0x33,
    'Quote': 0x34, 'Backquote': 0x35, 'Comma': 0x36, 'Period': 0x37, 'Slash': 0x38, 'CapsLock': 0x39,
})
for i in range(12):
    EXPECTED['F%d' % (i + 1)] = 0x3A + i
EXPECTED.update({
    'PrintScreen': 0x46, 'ScrollLock': 0x47, 'Pause': 0x48, 'Insert': 0x49, 'Home': 0x4A, 'PageUp': 0x4B,
    'Delete': 0x4C, 'End': 0x4D, 'PageDown': 0x4E, 'ArrowRight': 0x4F, 'ArrowLeft': 0x50, 'ArrowDown': 0x51,
    'ArrowUp': 0x52, 'NumLock': 0x53, 'NumpadDivide': 0x54, 'NumpadMultiply': 0x55, 'NumpadSubtract': 0x56,
    'NumpadAdd': 0x57, 'NumpadEnter': 0x58,
})
for i, c in enumerate('123456789'):
    EXPECTED['Numpad' + c] = 0x59 + i
EXPECTED.update({
    'Numpad0': 0x62, 'NumpadDecimal': 0x63, 'IntlBackslash': 0x64, 'ContextMenu': 0x65, 'NumpadEqual': 0x67,
})
for i in range(12):
    EXPECTED['F%d' % (13 + i)] = 0x68 + i
EXPECTED.update({
    'IntlRo': 0x87, 'KanaMode': 0x88, 'IntlYen': 0x89, 'Convert': 0x8A, 'NonConvert': 0x8B,
    'Lang1': 0x90, 'Lang2': 0x91,
    'ControlLeft': 0xE0, 'ShiftLeft': 0xE1, 'AltLeft': 0xE2, 'MetaLeft': 0xE3,
    'ControlRight': 0xE4, 'ShiftRight': 0xE5, 'AltRight': 0xE6, 'MetaRight': 0xE7,
})


def extract_keymap(text):
    m = re.search(r'const\s+KEYMAP\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        raise ValueError('KEYMAP not found in app.js')
    pairs = re.findall(r"""['"]([^'"]+)['"]\s*:\s*(0x[0-9a-fA-F]+|\d+)""", m.group(1))
    keymap = {}
    for code, value in pairs:
        if code in keymap:
            raise ValueError('duplicate code in KEYMAP: ' + code)
        keymap[code] = int(value, 0)
    return keymap


def check(text):
    """Returns a list of problems (empty = ok)."""
    try:
        keymap = extract_keymap(text)
    except ValueError as e:
        return [str(e)]
    problems = []
    for code, usage in EXPECTED.items():
        if code not in keymap:
            problems.append('missing key: %s' % code)
        elif keymap[code] != usage:
            problems.append('mismatch for %s: expected 0x%02X, got 0x%02X' % (code, usage, keymap[code]))
    for code in keymap:
        if code not in EXPECTED:
            problems.append('unexpected key in app.js: %s' % code)
    seen = {}
    for code, usage in keymap.items():
        if not (0x04 <= usage <= 0xA4 or 0xE0 <= usage <= 0xE7):
            problems.append('usage out of the range the server accepts for %s: 0x%02X' % (code, usage))
        if usage in seen:
            problems.append('duplicate usage 0x%02X for %s and %s' % (usage, code, seen[usage]))
        seen[usage] = code
    return problems


def mutants(text):
    """(name, mutated text): each is a one-place change to app.js that must be detected."""
    out = []

    def change_value(code, new):
        return re.sub(r"('%s'\s*:\s*)0x[0-9a-fA-F]+" % code, lambda m: m.group(1) + new, text, count=1)

    def drop(code):
        return re.sub(r"'%s'\s*:\s*0x[0-9a-fA-F]+,?\s*" % code, '', text, count=1)

    for code, new in [('KeyA', '0x05'), ('Digit0', '0x1E'), ('Enter', '0x29'), ('F1', '0x3B'), ('F12', '0x46'),
                      ('ShiftLeft', '0xE2'), ('ControlRight', '0xE0'), ('ArrowUp', '0x51'), ('Numpad0', '0x61'),
                      ('IntlBackslash', '0x87'), ('IntlRo', '0x64'), ('CapsLock', '0x3A'), ('ContextMenu', '0x66'),
                      ('Space', '0xA5'), ('MetaRight', '0xE8')]:
        out.append(('change %s to %s' % (code, new), change_value(code, new)))
    for code in ['KeyB', 'Digit1', 'Space', 'ArrowUp', 'CapsLock', 'Convert', 'ControlLeft', 'F24', 'NumpadEnter']:
        out.append(('drop %s' % code, drop(code)))
    out.append(('add an unexpected key', text.replace("'KeyA': 0x04,", "'KeyA': 0x04, 'Fn': 0x04,", 1)))
    out.append(('add a code twice', text.replace("'KeyA': 0x04,", "'KeyA': 0x04, 'KeyA': 0x04,", 1)))
    return [(n, t) for n, t in out if t != text]


def main():
    with open(APP_JS, encoding='utf-8') as f:
        text = f.read()
    problems = check(text)
    for p in problems:
        print('FAIL:', p)
    status = 1 if problems else 0
    if not problems:
        print('keymap ok (%d keys)' % len(extract_keymap(text)))

    if '--mutations' in sys.argv:
        survived = 0
        ms = mutants(text)
        for name, mutated in ms:
            detected = bool(check(mutated))
            print('%-8s %s' % ('KILLED' if detected else 'SURVIVED', name))
            survived += 0 if detected else 1
        print('%d mutants, %d survived' % (len(ms), survived))
        if survived:
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
