"""Keys between Windows and Linux. The wire carries a Windows key: virtual-key code, set-1 scancode
and an "extended" (E0) flag. Linux uses evdev codes, which equal the set-1 scancode for every
non-extended key up to F12; the rest are in KEYS below.
"""

# (evdev code, Windows VK, set-1 scancode, extended)
KEYS = [
    (1, 0x1B, 0x01, False), (14, 0x08, 0x0E, False), (15, 0x09, 0x0F, False), (28, 0x0D, 0x1C, False),
    (29, 0xA2, 0x1D, False), (42, 0xA0, 0x2A, False), (54, 0xA1, 0x36, False), (56, 0xA4, 0x38, False),
    (57, 0x20, 0x39, False), (58, 0x14, 0x3A, False), (69, 0x90, 0x45, True), (70, 0x91, 0x46, False),
    (12, 0xBD, 0x0C, False), (13, 0xBB, 0x0D, False), (26, 0xDB, 0x1A, False), (27, 0xDD, 0x1B, False),
    (39, 0xBA, 0x27, False), (40, 0xDE, 0x28, False), (41, 0xC0, 0x29, False), (43, 0xDC, 0x2B, False),
    (51, 0xBC, 0x33, False), (52, 0xBE, 0x34, False), (53, 0xBF, 0x35, False), (86, 0xE2, 0x56, False),
    (55, 0x6A, 0x37, False), (74, 0x6D, 0x4A, False), (78, 0x6B, 0x4E, False), (83, 0x6E, 0x53, False),
    (71, 0x67, 0x47, False), (72, 0x68, 0x48, False), (73, 0x69, 0x49, False), (75, 0x64, 0x4B, False),
    (76, 0x65, 0x4C, False), (77, 0x66, 0x4D, False), (79, 0x61, 0x4F, False), (80, 0x62, 0x50, False),
    (81, 0x63, 0x51, False), (82, 0x60, 0x52, False), (87, 0x7A, 0x57, False), (88, 0x7B, 0x58, False),
    # extended
    (96, 0x0D, 0x1C, True), (97, 0xA3, 0x1D, True), (98, 0x6F, 0x35, True), (99, 0x2C, 0x37, True),
    (100, 0xA5, 0x38, True), (102, 0x24, 0x47, True), (103, 0x26, 0x48, True), (104, 0x21, 0x49, True),
    (105, 0x25, 0x4B, True), (106, 0x27, 0x4D, True), (107, 0x23, 0x4F, True), (108, 0x28, 0x50, True),
    (109, 0x22, 0x51, True), (110, 0x2D, 0x52, True), (111, 0x2E, 0x53, True), (125, 0x5B, 0x5B, True),
    (126, 0x5C, 0x5C, True), (127, 0x5D, 0x5D, True),
    # no scancode worth trusting: matched by VK
    (119, 0x13, 0x45, False),  # Pause (its scancode is NumLock's)
    (113, 0xAD, 0x20, True), (114, 0xAE, 0x2E, True), (115, 0xAF, 0x30, True),
    (163, 0xB0, 0x19, True), (165, 0xB1, 0x10, True), (166, 0xB2, 0x24, True), (164, 0xB3, 0x22, True),
]
# Letters and digits: the VK is the character, the scancode is the evdev code.
_ROWS = [("QWERTYUIOP", 16), ("ASDFGHJKL", 30), ("ZXCVBNM", 44), ("1234567890", 2)]
for _row, _first in _ROWS:
    for _i, _ch in enumerate(_row):
        KEYS.append((_first + _i, ord(_ch), _first + _i, False))
for _i in range(10):  # F1-F10
    KEYS.append((59 + _i, 0x70 + _i, 59 + _i, False))

_BY_VK = {vk: ev for ev, vk, _, _ in KEYS}
_BY_SCAN = {(scan, ext): ev for ev, vk, scan, ext in KEYS if vk not in (0x13, 0x90)}
_TO_WIN = {ev: (vk, scan, ext) for ev, vk, scan, ext in KEYS}
_BY_VK_FIRST = {0x13, 0x90, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3}


def to_evdev(vk: int, scan: int, extended: bool):
    """The evdev code for a key from Windows, or None for a key Linux has no place for."""
    if vk in _BY_VK_FIRST:
        return _BY_VK.get(vk)
    if (scan, extended) in _BY_SCAN:
        return _BY_SCAN[(scan, extended)]
    if not extended and 1 <= scan <= 88:
        return scan
    return _BY_VK.get(vk)


def to_windows(evdev: int):
    """(vk, scan, extended) for a Linux key, or None."""
    return _TO_WIN.get(evdev)


if __name__ == "__main__":
    # Every key survives the round trip, and the awkward ones land where they should.
    for ev, vk, scan, ext in KEYS:
        assert to_evdev(*to_windows(ev)) == ev, (ev, to_windows(ev))
    assert to_evdev(0x13, 0x45, False) == 119  # Pause, not NumLock
    assert to_evdev(0x90, 0x45, True) == 69  # NumLock
    assert to_evdev(0x41, 0x1E, False) == 30  # A
    assert to_evdev(0x26, 0x48, True) == 103 and to_evdev(0x68, 0x48, False) == 72  # Up vs keypad 8
    assert to_windows(30) == (0x41, 0x1E, False)
    print("keymap: ok,", len(KEYS), "keys")
