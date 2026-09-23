#!/usr/bin/env python3
# Traegt BootBeacon.kext direkt nach Lilu in die OpenCore-config.plist ein.
# Aufruf: python3 add_to_config.py [/run/media/david/OPENCORE/EFI/OC/config.plist]
import plistlib, sys, shutil
p = sys.argv[1] if len(sys.argv) > 1 else "/run/media/david/OPENCORE/EFI/OC/config.plist"
shutil.copy(p, p + ".vor-bootbeacon")
c = plistlib.load(open(p, "rb"))
add = c["Kernel"]["Add"]
add[:] = [k for k in add if k.get("BundlePath") != "BootBeacon.kext"]
i = next(n for n, k in enumerate(add) if k.get("BundlePath") == "Lilu.kext")
add.insert(i + 1, {
    "Arch": "x86_64", "BundlePath": "BootBeacon.kext", "Comment": "Diagnose LED/Beep/NVRAM",
    "Enabled": True, "ExecutablePath": "Contents/MacOS/BootBeacon", "MaxKernel": "",
    "MinKernel": "", "PlistPath": "Contents/Info.plist"})
plistlib.dump(c, open(p, "wb"))
print("BootBeacon an Position", i + 1, "eingetragen. Backup:", p + ".vor-bootbeacon")
