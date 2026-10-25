#!/usr/bin/env python3
"""Write CHIMInjury_Animations.esp: an ESL-flagged plugin with only keywords.

Wound animation packs ship this plugin and use HasKeyword("CHIMInjury_Animations.esp"|0x800...) in their
OAR/DAR conditions. CHIMInjury.dll adds the keywords to a wounded actor's base form and removes them when the
wounds heal. Keep KEYWORDS in the same order as kAnimKeywords in SkyrimPlugin/src/main.cpp.

Usage: python tools/make_anim_plugin.py <output.esp>
"""
import struct
import sys

KEYWORDS = [
    "CHIMInjury_Anim_Concussion", "CHIMInjury_Anim_SkullBroken", "CHIMInjury_Anim_SkullSevere",
    "CHIMInjury_Anim_TorsoCut", "CHIMInjury_Anim_TorsoCutTreated", "CHIMInjury_Anim_RibsSoft", "CHIMInjury_Anim_RibsMiddle", "CHIMInjury_Anim_RibsSevere",
    "CHIMInjury_Anim_ArmLCut", "CHIMInjury_Anim_ArmLCutTreated", "CHIMInjury_Anim_ArmLBroken", "CHIMInjury_Anim_ArmLSplinted",
    "CHIMInjury_Anim_ArmRCut", "CHIMInjury_Anim_ArmRCutTreated", "CHIMInjury_Anim_ArmRBroken", "CHIMInjury_Anim_ArmRSplinted",
    "CHIMInjury_Anim_LegLCut", "CHIMInjury_Anim_LegLCutTreated", "CHIMInjury_Anim_LegLBroken", "CHIMInjury_Anim_LegLSplinted",
    "CHIMInjury_Anim_LegRCut", "CHIMInjury_Anim_LegRCutTreated", "CHIMInjury_Anim_LegRBroken", "CHIMInjury_Anim_LegRSplinted",
]
FORM_VERSION = 44  # Skyrim SE
ESL_FLAG = 0x200


def sub(tag: bytes, data: bytes) -> bytes:
    return tag + struct.pack("<H", len(data)) + data


def record(tag: bytes, flags: int, form_id: int, data: bytes) -> bytes:
    return tag + struct.pack("<IIIIHH", len(data), flags, form_id, 0, FORM_VERSION, 0) + data


def zstr(text: str) -> bytes:
    return text.encode("ascii") + b"\0"


def build() -> bytes:
    # Master 0 = Skyrim.esm, so our own forms are 0x01000800...
    kywd = b"".join(
        record(b"KYWD", 0, 0x01000800 + i, sub(b"EDID", zstr(name)) + sub(b"CNAM", struct.pack("<BBBB", 200, 60, 60, 0)))
        for i, name in enumerate(KEYWORDS)
    )
    group = b"GRUP" + struct.pack("<I4sIHH", 24 + len(kywd), b"KYWD", 0, 0, 0) + struct.pack("<I", 0) + kywd
    header = (
        sub(b"HEDR", struct.pack("<fII", 1.71, len(KEYWORDS) + 1, 0x800 + len(KEYWORDS)))  # records + 1 group
        + sub(b"CNAM", zstr("Marcius3d"))
        + sub(b"SNAM", zstr("CHIM Injury Framework: wound state keywords for animation packs"))
        + sub(b"MAST", zstr("Skyrim.esm"))
        + sub(b"DATA", struct.pack("<Q", 0))
    )
    return record(b"TES4", ESL_FLAG, 0, header) + group


if __name__ == "__main__":
    data = build()
    with open(sys.argv[1] if len(sys.argv) > 1 else "CHIMInjury_Animations.esp", "wb") as f:
        f.write(data)
    print(f"{len(KEYWORDS)} keywords, {len(data)} bytes")
