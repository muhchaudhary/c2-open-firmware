# Licensing

What the firmware images (`firmware/images/`, `firmware/build/`) contain,
and under which terms. They contain no GameSir code: `firmware/mkimage.py`
packs them from our code and the JieLi SDK only (docs/plan.md 6.1).

| Part | Where it comes from | Licence |
|---|---|---|
| The gamepad firmware: `firmware/src`, `firmware/boards` | this repository | Apache-2.0 (`LICENSE`) |
| SDK sources compiled into the app (`apps/`, `cpu/br23/`; the 16 files `firmware/sdk/sync.sh` patches among them), the SDK's prebuilt libraries (`cpu/br23/liba/*.a`, `include_lib`) | [Jieli-Tech/fw-AC63_BT_SDK](https://github.com/Jieli-Tech/fw-AC63_BT_SDK) at `firmware/sdk/SDK_COMMIT` | Apache-2.0 (`LICENSES/Apache-2.0.txt`, the SDK's `LICENSE`) |
| `uboot.boot` (the pad's second-stage loader) and `cfg_tool.bin`, copied into the image as they are; their sha256 is pinned in `firmware/mkimage.py` | the same SDK, `cpu/br23/tools/` | Apache-2.0 |
| `libc.a`, `libm.a` | JieLi's toolchain (flake.nix `jl-toolchain`); built from newlib | newlib's (permissive, BSD-style: newlib's `COPYING.NEWLIB`) |
| `libcompiler-rt.a` | the same toolchain; LLVM compiler-rt (clang 4.0) | UIUC / MIT (dual) |

Not code: the pad image's header keeps the name and version string the
stock images carry (`GS_C2_ADC_DEVICE`, `3.56`: `firmware/boards/c2/image.json`)
so tools and GameSir Connect see the same device, and the firmware presents
other vendors' USB IDs where hosts pick their driver by them (Sony's in DS4
mode, `firmware/src/core/gp_ds4.h`; the SInput ID, `firmware/boards/c2/board_c2_cfg.h`).

Nothing from `tools/` or `fw-re/` goes into the images. The flasher's
engine, kagaimiq's [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool)
(MIT), isn't in this repository: `flake.nix` and `fw-re/get-uboot-tool.sh`
fetch it at a pinned commit. It carries JieLi's USB loader blob, which the
flasher sends to the chip's RAM; JieLi gives no licence for that blob, so
we don't redistribute it.

## Our own code

Apache-2.0 (`LICENSE`), the same as the SDK it builds on: everything in this
repository. The pad's board file started from the SDK's example
(`firmware/boards/c2/board_c2.c`) and stays under the SDK's Apache-2.0. Released images carry this file and
`LICENSES/Apache-2.0.txt`, as Apache-2.0 section 4 asks.
