# Dependency provenance and release boundary

FrameYap's original code is [MIT licensed](../LICENSE), as selected by the project
owner. This does not relicense external models, fonts, protocols or runtimes.
There is no dependency on another application's checkout, assets or environment.

## Included source

`protocol/gamescope-input-method.xml` is the unmodified public Gamescope
**3.16.28** protocol, downloaded from
<https://github.com/ValveSoftware/gamescope/blob/3.16.28/protocol/gamescope-input-method.xml>.
SHA-256: `da35711f5d1d750bc47931132a89bf34e6b96a72bafc054d34092d3f42358ec4`.
Its embedded permissive copyright/license notice is preserved. Generated bindings
are build outputs, not hand-written wire encoding. The private protocol may change
with SteamOS; compatibility must be rechecked.

Frame controller bindings were authored here using the observed public input
profile names (`frame_controller`, `/input/grip`, `click`); no SteamVR driver code,
images, protected kernels or another application's assets were extracted for these bindings.

## Bundled font and UI reference

`assets/fonts/Inconsolata-Regular.ttf` is an unmodified copy of the typeface used
by kouseki's editor and VR canvas, extracted from its `assets/fonts` directory at
checkout revision `738569f4c41ff4c8fc9edd5bfff9c861957ea39e`.
SHA-256: `e0267abf9d734e2b9f766f8cb7a496b552c57cdfeacfa0efdc5bfd21940ae145`.
Copyright 2006 The Inconsolata Project Authors; **SIL Open Font License 1.1**,
retained in `assets/fonts/OFL-Inconsolata.txt` (line endings and trailing whitespace
normalized; license text unchanged). The reviewed OFL permits
bundling and redistribution with its copyright/license notice; the font remains
OFL, not MIT, and is not sold by itself. Upstream: <https://github.com/googlefonts/Inconsolata>.
The TTF is unchanged. No MSDF atlas, icons, engine code or renderer dependencies
were copied. Unicode coverage is finite; missing glyphs use the face's notdef glyph.

Visual references: kouseki's `apps/vr_workspace/hud.cpp` (rounded mint-to-blue
perimeter and curved accent) and `menu_tablet.hpp` (dark cards, highlighted
selection). FrameYap implements those design ideas independently on a single
CPU RGBA surface. Building, installing and running require no kouseki checkout.
CMake installs the font and OFL with assets; native staging defaults to that
font, places the launcher copy at `fonts/font.ttf`, and includes its license in
`THIRD_PARTY_NOTICES.txt`. Custom staging fonts still require an explicit license.

## Explicit native build inputs (not vendored)

- Valve OpenVR SDK v2.15.6: BSD-3-Clause-style license, copyright Valve 2015;
  retain its LICENSE with redistributed loader binaries.
- SDL3: zlib license; device trial used SDL 3.2.16 built in a private user prefix.
- Wayland client and scanner: retain upstream MIT-style notices.
- FreeType: choose and comply with its applicable FTL/GPL licensing option.
- Optional font override: the earlier device check used system Hack Regular.
  A custom release font must include its own license and assessed glyph coverage;
  new builds default to the bundled Inconsolata described above.
- Compiler runtime, libc minimum and transitive shared libraries require a release
  dependency audit. Passing a developer build is not a portable-runtime guarantee.

## Redux weights and proprietary runtime are different

Public model: <https://huggingface.co/moondream/parakeet-redux>, exact revision
`fad622f25f303105c20d70e201bcc477c88b620c`, model card **CC-BY-4.0**. Attribution:
Moondream/M87 Labs, Parakeet Redux, derived from NVIDIA Parakeet TDT 0.6B v3.
No modifications to the supplied weights are made. Exact model/config/tokenizer
sizes and SHA-256 hashes are recorded in `python/frameyap/model_files.py`.
`fetch-model.py` fetches and retains the original model card alongside the files.
No weights are committed to this repository.

The inspected `kestrel-kernels==0.7.0` wheel license identifies it as proprietary
M87 Labs software and says use requires a separate written agreement. Copying and
redistribution are restricted by that agreement. This includes its protected CPU
payload, not just CUDA. The Python wrapper and model card do not override those
terms. No attempt was made to unpack/decrypt/reverse-engineer protected kernels.

**Release blocker:** permission covering use and redistribution has not been
established here. The initial on-device compatibility measurements preceded this
license review; further inference/bundling was paused when the issue was found.
The acquired packages remain isolated under the device's project-owned development
directory, not in Git or a published artifact. Do not advertise the GitHub runtime
bundle as available or automatically download/install those packages for end users.
Resolve permission with the vendor, or separately scope an independently licensed
runtime for the same weights. Do not silently substitute a dense/heavier model.

Other runtime packages (Torch CPU, numpy, tokenizer/native extensions, etc.) also
need their own notice/license inventory before publication. The public model's
small size is neither total runtime size nor redistribution permission.

## CPU trial dependency choice

Trial interface pins: moondream **2.4.0**, kestrel **0.8.0**, kernels **0.7.0**,
native **0.1.8**, Python **3.12.3**, Torch **2.8.0+cpu** on ARM64. An unqualified
moondream install initially resolved a CUDA-enabled Torch and NVIDIA wheels;
these were replaced/removed from the owned venv before measurement. The measured
Torch reported `torch.version.cuda is None`. Do not repeat an unconstrained
`pip install moondream` as a CPU setup recipe.

For eventual packaging research, a standalone CPython 3.12.14 ARM64 distribution
was downloaded but not bundled with the proprietary runtime:
<https://github.com/astral-sh/python-build-standalone/releases/tag/20260901>,
`cpython-3.12.14+20260901-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz`,
SHA-256 `577b4bec0793ad1ff0cbff9adbd0df078eddde38a4c41bf5d83ad381a85ee39d`.
Its included licenses and compatible native dependencies still require review
before any release. No public prebuilt FrameYap release has been published.
