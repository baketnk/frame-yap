# Third-party components

FrameYap's own code is under [LICENSE](../LICENSE). Everything below is fetched
from, or built against, its upstream project; each keeps its own license. This is
a pointer list, not a legal audit. No prebuilt archive is published: v0.1 is
source-only, and Python packages are fetched from PyPI on the user's machine.

## Included in this repository

| Item | Upstream | License |
| --- | --- | --- |
| `protocol/gamescope-input-method.xml` (Gamescope 3.16.28, unmodified, SHA-256 `da35711f5d1d750bc47931132a89bf34e6b96a72bafc054d34092d3f42358ec4`) | <https://github.com/ValveSoftware/gamescope> | Permissive notice embedded in the file |
| `assets/fonts/Inconsolata-Regular.ttf` (unchanged, SHA-256 `e0267abf9d734e2b9f766f8cb7a496b552c57cdfeacfa0efdc5bfd21940ae145`) | <https://github.com/googlefonts/Inconsolata> | SIL OFL 1.1, text in `assets/fonts/OFL-Inconsolata.txt` |

## Native build dependencies (system or user-supplied)

| Component | Upstream | License |
| --- | --- | --- |
| OpenVR SDK 2.15.6 | <https://github.com/ValveSoftware/openvr> | BSD-3-Clause-style |
| SDL3 | <https://github.com/libsdl-org/SDL> | zlib |
| Wayland (client, scanner) | <https://gitlab.freedesktop.org/wayland/wayland> | MIT |
| libxcb | <https://gitlab.freedesktop.org/xorg/lib/libxcb> | MIT-style |
| FreeType 2 | <https://freetype.org> | FreeType Project License (FTL) or GPL-2.0; FrameYap uses FTL |
| Vulkan loader and drivers | System | Vary by vendor |

## Voice recognition runtime (fetched by the user's install)

| Component | Where | License |
| --- | --- | --- |
| Parakeet Redux weights, revision `fad622f25f303105c20d70e201bcc477c88b620c`, derived from NVIDIA Parakeet TDT 0.6B v3 by Moondream/M87 Labs; pinned sizes and hashes in `assets/backends/redux.json` | <https://huggingface.co/moondream/parakeet-redux> | CC-BY-4.0 |
| Kestrel, `kestrel-kernels` | <https://github.com/m87-labs/kestrel> | Local inference is free and needs no API key, per the Kestrel README. Finetuned-model inference needs an API key and is not used here. |
| `moondream` | PyPI | See the package |
| PyTorch (CPU build) | <https://github.com/pytorch/pytorch> | BSD-3-Clause |
| NumPy | <https://github.com/numpy/numpy> | BSD-3-Clause and bundled notices |
| Hugging Face `tokenizers` | <https://github.com/huggingface/tokenizers> | Apache-2.0 |

Versions seen working on Frame (ARM64, Python 3.12.3): moondream 2.4.0, kestrel
0.8.0, kestrel-kernels 0.7.0, native 0.1.8, Torch 2.8.0+cpu. These are observed
versions, not tested minimums. An unconstrained `pip install moondream` resolved
CUDA-enabled Torch and NVIDIA wheels, so the CPU Torch wheel must be selected
explicitly.

Model weights are downloaded only on an explicit request and are never committed.
