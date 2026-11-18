#!/usr/bin/env python3
"""Checks that the firmware, both companion apps and the docs agree on Pip Link.

Run: python tools/check_companion.py
No Bluetooth or board needed: it reads the sources and the built artifacts.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ok = True


def check(name, cond, detail=""):
    global ok
    print(("ok    " if cond else "FAIL  ") + name + ("" if cond else "  " + str(detail)))
    ok &= bool(cond)


def read(path):
    return open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read()


def uuids(text, pattern):
    return [m.group(1).lower() for m in re.finditer(pattern, text, re.I)]


ble = read("firmware/main/ble.c")
android = "\n".join(
    read(os.path.join("apps/android/app/src/main/java/com/novikov/piplink", f))
    for f in sorted(os.listdir(os.path.join(ROOT, "apps/android/app/src/main/java/com/novikov/piplink")))
    if f.endswith(".kt")
)
windows = read("apps/windows/pip_link.py")
doc = read("docs/companion.md")

fw = uuids(ble, r'uuid\(&COMP(?:_RX|_TX|_INFO)?,\s*"([0-9a-f-]{36})"')
an = uuids(android, r'UUID\.fromString\("([0-9a-f-]{36})"')
docuu = uuids(doc, r"`(50495044-[0-9a-f-]{27})`")
check("firmware defines 4 Pip Link UUIDs", len(fw) == 4, fw)
check("Android has the same service/RX/TX", an[:3] == fw[:3], (an[:3], fw[:3]))
check("docs list the same 4 UUIDs", docuu[:4] == fw, (docuu[:4], fw))
check("Windows has the same service/RX/TX", all(u in windows.lower() for u in fw[:3]), fw[:3])

check("firmware serves the INFO string", "Pip companion 1" in ble)
for word in ("HELLO", "TIME", "NOTE", "CALL", "TRACK", "PRIVACY"):
    check(f"firmware understands {word}", word in ble)
for word in ("HELLO", "TIME", "NOTE", "TRACK"):
    check(f"both apps send {word}", word in android and word in windows, (word,))
check("Android sends CALL (calls are Android-only)", "CALL " in android)
check("Windows says calls are unavailable", "calls are not available on Windows" in windows)

ver = re.search(r'PROJECT_VER "([^"]+)"', read("firmware/CMakeLists.txt")).group(1)
bundle = read("docs/firmware/firmware.js")
m = re.search(r"window\.PIP_FIRMWARE = (\{.*\});", bundle, re.S)
check("docs/firmware/firmware.js is a JSON bundle", m is not None)
if m:
    data = json.loads(m.group(1))
    check("panel firmware version matches PROJECT_VER", data.get("version") == ver, (data.get("version"), ver))
    check("panel firmware has all parts", len(data.get("parts", [])) >= 4, len(data.get("parts", [])))

for path, minimum in (
    ("firmware/build/keypet.bin", 1_000_000),
    ("apps/android/PipLink.apk", 1_000_000),
    ("apps/windows/dist/PipLink/PipLink.exe", 1_000_000),
):
    p = os.path.join(ROOT, path)
    check(f"artifact {path}", os.path.exists(p) and os.path.getsize(p) >= minimum,
          os.path.getsize(p) if os.path.exists(p) else "missing")

print("ok" if ok else "FAILED")
sys.exit(0 if ok else 1)
