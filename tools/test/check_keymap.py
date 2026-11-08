import sys
import re
import unittest

def extract_keymap(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    match = re.search(r'const\s+KEYMAP\s*=\s*({[^}]+});', content)
    if not match:
        raise ValueError("Could not find KEYMAP in app.js")

    keymap_str = match.group(1)

    keymap = {}
    for line in keymap_str.split('\n'):
        line = line.strip()
        if not line or line.startswith('//'):
            continue

        matches = re.finditer(r'''['"]([^'"]+)['"]\s*:\s*(0x[0-9a-fA-F]+|\d+)''', line)
        for m in matches:
            code = m.group(1)
            usage = int(m.group(2), 0)
            keymap[code] = usage
    return keymap

EXPECTED_KEYMAP_ORIGINAL = {
    # Letters
    'KeyA': 0x04, 'KeyB': 0x05, 'KeyC': 0x06, 'KeyD': 0x07,
    'KeyE': 0x08, 'KeyF': 0x09, 'KeyG': 0x0A, 'KeyH': 0x0B,
    'KeyI': 0x0C, 'KeyJ': 0x0D, 'KeyK': 0x0E, 'KeyL': 0x0F,
    'KeyM': 0x10, 'KeyN': 0x11, 'KeyO': 0x12, 'KeyP': 0x13,
    'KeyQ': 0x14, 'KeyR': 0x15, 'KeyS': 0x16, 'KeyT': 0x17,
    'KeyU': 0x18, 'KeyV': 0x19, 'KeyW': 0x1A, 'KeyX': 0x1B,
    'KeyY': 0x1C, 'KeyZ': 0x1D,

    # Numbers
    'Digit1': 0x1E, 'Digit2': 0x1F, 'Digit3': 0x20, 'Digit4': 0x21,
    'Digit5': 0x22, 'Digit6': 0x23, 'Digit7': 0x24, 'Digit8': 0x25,
    'Digit9': 0x26, 'Digit0': 0x27,

    # Modifiers
    'ControlLeft': 0xE0, 'ShiftLeft': 0xE1, 'AltLeft': 0xE2, 'MetaLeft': 0xE3,
    'ControlRight': 0xE4, 'ShiftRight': 0xE5, 'AltRight': 0xE6, 'MetaRight': 0xE7,

    # Control/Navigation
    'Enter': 0x28, 'Escape': 0x29, 'Backspace': 0x2A, 'Tab': 0x2B, 'Space': 0x2C,

    # Symbols
    'Minus': 0x2D, 'Equal': 0x2E, 'BracketLeft': 0x2F, 'BracketRight': 0x30,
    'Backslash': 0x31, 'Semicolon': 0x33, 'Quote': 0x34, 'Backquote': 0x35,
    'Comma': 0x36, 'Period': 0x37, 'Slash': 0x38,

    # F keys
    'F1': 0x3A, 'F2': 0x3B, 'F3': 0x3C, 'F4': 0x3D, 'F5': 0x3E, 'F6': 0x3F,
    'F7': 0x40, 'F8': 0x41, 'F9': 0x42, 'F10': 0x43, 'F11': 0x44, 'F12': 0x45,

    # System/Navigation keys
    'PrintScreen': 0x46, 'ScrollLock': 0x47, 'Pause': 0x48, 'Insert': 0x49,
    'Home': 0x4A, 'PageUp': 0x4B, 'Delete': 0x4C, 'End': 0x4D, 'PageDown': 0x4E,
    'ArrowRight': 0x4F, 'ArrowLeft': 0x50, 'ArrowDown': 0x51, 'ArrowUp': 0x52,

    # Numpad
    'NumLock': 0x53,
    'NumpadDivide': 0x54, 'NumpadMultiply': 0x55, 'NumpadSubtract': 0x56, 'NumpadAdd': 0x57,
    'NumpadEnter': 0x58,
    'Numpad1': 0x59, 'Numpad2': 0x5A, 'Numpad3': 0x5B, 'Numpad4': 0x5C,
    'Numpad5': 0x5D, 'Numpad6': 0x5E, 'Numpad7': 0x5F, 'Numpad8': 0x60,
    'Numpad9': 0x61, 'Numpad0': 0x62, 'NumpadDecimal': 0x63,

    # Misc
    'ContextMenu': 0x65,

    # JIS specific
    'IntlYen': 0x89, 'IntlRo': 0x87, 'IntlBackslash': 0x87, 'KanaMode': 0x88,
    'Convert': 0x8A, 'NonConvert': 0x8B,
}

EXPECTED_KEYMAP = EXPECTED_KEYMAP_ORIGINAL.copy()

class TestKeymap(unittest.TestCase):
    def setUp(self):
        self.keymap = extract_keymap('components/web_server/web/app.js')

    def test_complete_match(self):
        for code, expected_usage in EXPECTED_KEYMAP.items():
            self.assertIn(code, self.keymap, f"Missing key: {code}")
            self.assertEqual(self.keymap[code], expected_usage, f"Mismatch for {code}: expected {hex(expected_usage)}, got {hex(self.keymap[code])}")

        for code in self.keymap:
            self.assertIn(code, EXPECTED_KEYMAP, f"Unexpected key in app.js: {code}")

    def test_valid_ranges(self):
        for code, usage in self.keymap.items():
            is_normal = (0x04 <= usage <= 0xA4)
            is_modifier = (0xE0 <= usage <= 0xE7)
            self.assertTrue(is_normal or is_modifier, f"Usage out of bounds for {code}: {hex(usage)}")

    def test_no_duplicates(self):
        seen = {}
        allowed_duplicates = {'IntlBackslash': 0x87, 'IntlRo': 0x87}

        for code, usage in self.keymap.items():
            if code in allowed_duplicates:
                continue
            self.assertNotIn(usage, seen, f"Duplicate usage {hex(usage)} for {code} and {seen.get(usage)}")
            seen[usage] = code

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--mutations':
        print("Running mutation tests...")

        mutations = [
            ("Change KeyA value", lambda: {**EXPECTED_KEYMAP_ORIGINAL, 'KeyA': 0x99}),
            ("Remove KeyB", lambda: {k:v for k,v in EXPECTED_KEYMAP_ORIGINAL.items() if k != 'KeyB'}),
            ("Change Enter value", lambda: {**EXPECTED_KEYMAP_ORIGINAL, 'Enter': 0x29}),
            ("Remove ArrowUp", lambda: {k:v for k,v in EXPECTED_KEYMAP_ORIGINAL.items() if k != 'ArrowUp'}),
            ("Change F1 value", lambda: {**EXPECTED_KEYMAP_ORIGINAL, 'F1': 0x3B}),
            ("Remove Digit1", lambda: {k:v for k,v in EXPECTED_KEYMAP_ORIGINAL.items() if k != 'Digit1'}),
            ("Change ShiftLeft", lambda: {**EXPECTED_KEYMAP_ORIGINAL, 'ShiftLeft': 0xE2}),
            ("Remove Space", lambda: {k:v for k,v in EXPECTED_KEYMAP_ORIGINAL.items() if k != 'Space'}),
            ("Change ContextMenu", lambda: {**EXPECTED_KEYMAP_ORIGINAL, 'ContextMenu': 0x66}),
            ("Remove Convert", lambda: {k:v for k,v in EXPECTED_KEYMAP_ORIGINAL.items() if k != 'Convert'})
        ]

        for name, mutator in mutations:
            EXPECTED_KEYMAP = mutator()
            try:
                suite = unittest.TestLoader().loadTestsFromTestCase(TestKeymap)
                result = unittest.TextTestRunner(stream=sys.stderr, verbosity=0).run(suite)
                if not result.wasSuccessful():
                    print(f"{name}: Failed as expected")
                else:
                    print(f"{name}: Did NOT fail!")
            except Exception as e:
                print(f"{name}: Error {e}")

        EXPECTED_KEYMAP = EXPECTED_KEYMAP_ORIGINAL
        sys.exit(0)

    unittest.main()
