#!/usr/bin/env python3
"""Stage a native-only release from an explicit native build and licensed files.

No downloads, compiler invocation, ASR runtime, registration or launch.
System Vulkan loader/driver, Wayland/FreeType/libstdc++/glibc remain platform prerequisites.
"""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--destination", type=Path, required=True, help="new staging directory")
    for name in ("openvr-library", "openvr-license", "sdl-library", "sdl-license"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--font", type=Path, help="override bundled Inconsolata (requires --font-license)")
    p.add_argument("--font-license", type=Path)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    if bool(args.font) != bool(args.font_license):
        p.error("a font override requires both --font and --font-license")
    args.font = args.font or root / "assets/fonts/Inconsolata-Regular.ttf"
    args.font_license = args.font_license or root / "assets/fonts/OFL-Inconsolata.txt"
    dest = args.destination.absolute()
    if dest.exists() or dest.is_symlink():
        p.error("destination already exists; use a new staging directory")
    for name in ("openvr_library", "openvr_license", "sdl_library", "sdl_license", "font", "font_license"):
        if not getattr(args, name).is_file():
            p.error(f"missing explicit input {name}")
    cache = (args.build / "CMakeCache.txt").read_text()
    if "FRAMEYAP_NATIVE:BOOL=ON" not in cache:
        p.error("build must explicitly enable FRAMEYAP_NATIVE")
    dest.mkdir(mode=0o700, parents=True)
    subprocess.run(["cmake", "--install", str(args.build.absolute()), "--prefix", str(dest)], check=True)
    # The panel can launch this installer as a child after an explicit model
    # consent click. It is self-contained (heredoc payload), not a network stub.
    shutil.copyfile(root / "install.sh", dest / "bin/install.sh")
    (dest / "bin/install.sh").chmod(0o755)
    for directory in ("lib", "fonts", "licenses"):
        (dest / directory).mkdir()
    shutil.copyfile(args.openvr_library, dest / "lib/libopenvr_api.so", follow_symlinks=True)
    shutil.copyfile(args.sdl_library, dest / "lib/libSDL3.so.0", follow_symlinks=True)
    shutil.copyfile(args.font, dest / "fonts/font.ttf")
    notices = ["FrameYap native-only release. No ASR runtime or model is included.\n",
               "Original FrameYap code: MIT. System Vulkan/Wayland/FreeType/libstdc++/glibc are not bundled.\n",
               "This software is based in part on the work of the FreeType Team. "
               "FreeType is a system dynamic library, not bundled in this native-only stage.\n",
               "Bundled libraries: Valve OpenVR and unmodified SDL3; font license included below.\n",
               "This package does not include Kestrel or provide a functioning ASR environment.\n"]
    for label, file in (("FrameYap", root / "LICENSE"), ("OpenVR", args.openvr_license),
                        ("SDL3", args.sdl_license), ("Font", args.font_license)):
        notices.extend([f"\n--- {label} ---\n", file.read_text()])
    notices.extend(["\n--- Gamescope protocol: embedded copyright/license ---\n",
                    (root / "protocol/gamescope-input-method.xml").read_text()])
    (dest / "licenses/THIRD_PARTY_NOTICES.txt").write_text("\n".join(notices))
    print(f"Native-only stage: {dest}. Package with --external-runtime; do not claim bundled ASR.")


if __name__ == "__main__":
    main()
