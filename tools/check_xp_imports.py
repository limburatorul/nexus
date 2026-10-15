"""Fails the build when nexus.exe imports a function Windows XP SP3 does not export.

xp_imports.txt lists every DLL!function checked present on XP SP3. A new import has to be checked
against XP (MSDN's "Minimum supported client", or the DLL on a real XP) and added there by hand;
otherwise the exe would refuse to start on XP with "procedure entry point not found".
"""

import os
import subprocess
import sys

exe = sys.argv[1]
allowed = set()
with open(os.path.join(os.path.dirname(__file__), "xp_imports.txt")) as f:
    for line in f:
        line = line.split("#")[0].strip()
        if line:
            allowed.add(line.lower())

dump = subprocess.run(["objdump", "-p", exe], capture_output=True, text=True, check=True).stdout
used, dll = set(), None
for line in dump.splitlines():
    s = line.strip()
    if s.startswith("DLL Name:"):
        dll = s.split(":", 1)[1].strip().lower()
    elif dll and len(s.split()) == 4 and s.split()[1] == "<none>":  # "<vma> <none> <hint> <name>"
        used.add(dll + "!" + s.split()[3].lower())
    elif s.startswith("The Export Tables") or s.startswith("PE File Base Relocations"):
        dll = None

unknown = sorted(used - allowed)
for name in unknown:
    print("not checked for XP:", name)
print(len(used), "imports,", len(unknown), "not in tools/xp_imports.txt")
sys.exit(1 if unknown else 0)
