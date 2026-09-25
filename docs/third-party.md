# Dependency provenance and release boundary

FrameYap's original code is [MIT licensed](../LICENSE). This does not relicense
models, fonts, protocols, native libraries or Python wheels. There is no dependency
on another application's checkout, assets or environment. **This is an upstream
license inventory, not an audit of a particular release binary. No public release
has been published.** Before shipping any archive, inspect the actual staged
files, their transitive dependencies and notices, and test on a clean supported
host. An external dependency does not make the current archive self-contained.

## Included source/assets

- `protocol/gamescope-input-method.xml`: unmodified public Gamescope **3.16.28**
  protocol, <https://github.com/ValveSoftware/gamescope/blob/3.16.28/protocol/gamescope-input-method.xml>.
  SHA-256 `da35711f5d1d750bc47931132a89bf34e6b96a72bafc054d34092d3f42358ec4`;
  embedded permissive copyright/license notice preserved. Generated bindings are
  build outputs; this private Gamescope extension needs rechecking after updates.
- `assets/fonts/Inconsolata-Regular.ttf`: unchanged Inconsolata Regular from
  <https://github.com/googlefonts/Inconsolata>, copyright 2006 The Inconsolata
  Project Authors, **SIL OFL 1.1**. SHA-256
  `e0267abf9d734e2b9f766f8cb7a496b552c57cdfeacfa0efdc5bfd21940ae145`.
  `assets/fonts/OFL-Inconsolata.txt` retains the license. The font remains OFL,
  not MIT, and is not sold by itself. An override font requires its own license.
- Frame controller bindings and the mint-to-blue CPU-rasterized panel were
  authored here, using public profile names; no SteamVR driver artwork, MSDF
  atlas, other app renderer or protected kernels were copied.

Native staging uses `scripts/stage-native.py`: it includes the chosen font/license
and copied SDL3 and OpenVR notices in `licenses/THIRD_PARTY_NOTICES.txt`.
It does **not** bundle ASR. A4 is not closed for release: inspect the final
notice file, especially FreeType attribution, plus the selected native binary
and any bundled wheel/licenses before publishing.

## Native build/runtime inventory

| Component | Upstream license / evidence | Current packaging boundary / action |
| --- | --- | --- |
| Valve OpenVR SDK 2.15.6 | BSD-3-Clause-style license, SDK LICENSE. | Staging copies its loader and explicitly supplied license; verify chosen binary and transitive closure. |
| SDL3 (device trial 3.2.16) | zlib, local `/usr/share/licenses/sdl3/LICENSE`. | Staging copies explicit library and license. Confirm exact build options/version and its transitive libraries. |
| Wayland client + scanner | MIT/Expat-style, local `/usr/share/licenses/wayland/COPYING`. | Client is linked from system; scanner is build-time. If shipped, include copyright/license and audit closure. |
| libxcb | MIT-style with name-use restriction, local `/usr/share/licenses/libxcb/COPYING`. | Xwayland focus guard uses client library at runtime; not bundled by current native stage. Audit exact binary. |
| FreeType 2 | **FreeType Project License (FTL) selected** for this project, local `/usr/share/licenses/freetype2/FTL.TXT`; upstream also offers a GPL option. | Current stage uses system library, not bundled. Credit FreeType Team for use; if distributing its binary, meet FTL binary disclaimer/notice obligations and review the precise build. Do not silently substitute GPL terms. |
| Vulkan loader, driver, system graphics dependencies | Loader/driver licenses vary by build and vendor. | Current stage depends on system Vulkan loader/driver; no GPU runtime is bundled. Audit the chosen loader if ever bundled. |
| Compiler runtime (`libstdc++`, `libgcc_s` when used) | GCC libraries: GPL with **GCC Runtime Library Exception** in upstream distribution; local `/usr/share/licenses/libstdc++/RUNTIME.LIBRARY.EXCEPTION` and `libgcc/...` are exception texts, not a full installed release audit. | Current stage relies on system runtime. Audit dynamic linkage, C++ ABI/`GLIBCXX_*` and exception coverage for *any* bundled compiler libraries; include corresponding complete notices/source obligations as applicable. |
| glibc/loader | GNU LGPL-2.1-or-later for core GNU C Library, with component-specific exceptions and other licenses to inspect. | Current stage relies on system libc/loader. Audit exact target binary symbol versions (`GLIBC_*`), ELF interpreter and its transitive closure; no minimum glibc/`GLIBCXX`/kernel floor is certified here. |

The historical Frame CPU trial used Torch **2.8.0+cpu** on a host with **glibc
2.39**. Those are *observed trial versions*, **not** minimum compatible versions
for FrameYap, Python wheels or a future released artifact. `ldd` on a developer
machine alone is not sufficient: inspect the staged ARM64 binaries with `readelf`
(`NEEDED`, ELF interpreter, symbol-version requirements), `ldd` on a trusted
clean target, actual bundled libraries and notices, and test the final archive on
a clean supported Frame. Check the chosen compiler, CPU instruction/kernel,
FreeType/Wayland/XCB/SDL/OpenVR/Vulkan ABI, Python/native wheels and licenses.
Do not invent a libc floor from a build host's version.

## Redux weights and inference runtime

Model: <https://huggingface.co/moondream/parakeet-redux>, exact revision
`fad622f25f303105c20d70e201bcc477c88b620c`, model card **CC-BY-4.0**.
Attribution: Moondream/M87 Labs, Parakeet Redux, derived from NVIDIA Parakeet TDT
0.6B v3. No modifications to the supplied weights are made. Pinned model/config/
tokenizer/card sizes and SHA-256 hashes, source and attribution are recorded in
`assets/backends/redux.json` (schema validated by `python/frameyap/model_files.py`),
not hardcoded in `model_files.py`. `fetch-model.py` fetches and retains the model
card alongside the weights on **explicit** request; neither build/tests nor a
normal app launch downloads them. `scripts/model-status.py` and the native
`--list-models` / `--check-model` interface inspect/hashes local files offline.
No weights are committed to Git.

The current Redux worker uses separately provisioned `moondream` Python and its
`kestrel` / `kestrel-kernels` dependencies, not a bundled runtime. Kestrel's
upstream README says “Local inference is free and requires no API key”
(<https://github.com/m87-labs/kestrel>); finetuned-model inference needs an API
key and is **not** this path. The native-only installer **does not run pip**,
provision an interpreter, or make a native-only artifact able to transcribe on
its own. A person supplying a Python environment must review/authorize its
exact dependency closure. No bundled-ASR artifact is licensed/approved by this
inventory.

| Python/native package | Upstream license inventory (not a wheel audit) | Release action |
| --- | --- | --- |
| PyTorch / Torch CPU | PyTorch project: BSD-3-Clause; third-party components/wheels carry additional notices and dependencies. | No Torch wheels bundled. Pin CPU-only ARM64 wheel if building a distribution; audit its `LICENSE`, `NOTICE`, `third_party`/wheel contents and `NEEDED`/symbol versions. |
| NumPy | NumPy core: BSD-3-Clause; dependencies/embedded algorithms carry additional BSD, MIT, 0BSD, zlib, CC0 and other notices depending on wheel. Local `python-numpy` 2.5.3 package metadata (`/usr/lib/python3.14/site-packages/numpy-2.5.3.dist-info/METADATA`) declares `BSD-3-Clause AND 0BSD AND MIT AND Zlib AND CC0-1.0` with many `License-File` entries (different from the historical trial environment). | Not bundled. Keep *all* license files and inspect the exact target wheel, BLAS/OpenBLAS and runtime closure before redistribution. |
| Hugging Face `tokenizers` | Upstream `huggingface/tokenizers` is Apache-2.0; native/Rust crate dependencies need separate inventory. | Not bundled. Confirm actual installed wheel version, package LICENSE/NOTICE and transitive Rust/native code if ever distributed. |
| `moondream`, Kestrel/kernels/native, Python interpreter | Distinct packages with distinct license files and native transitive code; Kestrel local-use statement is not a blanket redistribution license. | None bundled. Exact versions, permissions, wheel notices, CPython build and native linkage require review before any runtime bundle. |

Development CPU trial *interfaces*, not package-floor promises: moondream
**2.4.0**, kestrel **0.8.0**, kernels **0.7.0**, native **0.1.8**, Python
**3.12.3**, Torch **2.8.0+cpu** on ARM64. An unqualified moondream install
initially resolved CUDA-enabled Torch and NVIDIA wheels; the owned trial venv
was corrected before measurement (`torch.version.cuda is None`). Do **not**
repeat unconstrained `pip install moondream` as a CPU setup recipe.

A standalone CPython 3.12.14 ARM64 distribution was downloaded for packaging
research but **not bundled**: <https://github.com/astral-sh/python-build-standalone/releases/tag/20260901>,
`cpython-3.12.14+20260901-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz`,
SHA-256 `577b4bec0793ad1ff0cbff9adbd0df078eddde38a4c41bf5d83ad381a85ee39d`.
Its own licenses and native compatibility need review before any release.
