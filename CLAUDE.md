# CLAUDE.md

Guidance for Claude Code in this repository. Workspace-wide rules (language, bench, issue tracking) live in `../CLAUDE.md`.

This is `eumarcodev/pyrowave`, the user's fork of PyroWave: an intra-only wavelet video codec implemented in Vulkan
compute shaders, built for very low-latency game streaming. It is consumed as a submodule by Helios
(`third-party/pyrowave`, encoder, Windows) and Selene (`third_party/pyrowave`, decoder, Android via `pyrowave-android`).

## 1. Critical commands

```bash
bash checkout_granite.sh      # checks out only the Granite parts the standalone build needs
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=output -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja install                 # installs pyrowave.h, pyrowave-shared and its CMake/pkg-config files
./pyrowave-c-test             # unit test of the shared C API (also the API usage guide)
```

- On Windows run these inside MSYS2 UCRT64 (`C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "<cmd>"`).
- `pyrowave-c-test`, `pyrowave-c-interop-test` and `pyrowave-device-validation` are built only when this project is top level.
- Sample/CLI tools (`pyrowave-encode`, `pyrowave-decode`, viewer, sandbox) need the full Granite (`git submodule update --init --recursive Granite`) and `-DPYROWAVE_DEVEL=ON`.
- Android standalone: add `-DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<ver>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a`.
- After changing any `.comp`/`.frag`/`.vert` shader, regenerate the embedded headers with `slangmosh.sh` (needs the `slangmosh` tool).
- Real validation of a codec change is done through the consumers: Helios `test_sunshine` (`test_pyrowave_*`) and a Helios + Selene stream session.

## 2. Architecture map

- Public C API: `pyrowave.h` (implementation `pyrowave_c.cpp`, exports in `pyrowave-shared.def`, version script `link.T`). This is the only interface Helios and Selene use.
- Codec core: `pyrowave_encoder.*`, `pyrowave_decoder.*`, `pyrowave_common.*`, `pyrowave_config.hpp`.
- GPU code: `shaders/` (DWT/IDWT, quantization, rate control, block packing); `shaders/slangmosh*.hpp` are generated from `shaders/slangmosh*.json` and `Granite/video/slangmosh_encode.json`.
- Bitstream definition: `bitstream/bitstream.md`.
- Dev tools and evaluation: `encode.cpp`, `decode.cpp`, `bench.cpp`, `evaluator.cpp`, `psnr.cpp`, `viewer.cpp`, `sandbox.cpp`, `metrics/`, `eval-results/`, `pyrowave-eval-gen.py`.
- `Granite/`: pinned submodule providing the Vulkan backend, math and video scaler.
- Build options: `PYROWAVE_DEVEL`, `PYROWAVE_UTILS`, `PYROWAVE_FP32_STORAGE` (OFF), `PYROWAVE_FP32_MATH` (ON).

## 3. Hard rules

- **IMPORTANT: NEVER clone or update Granite with `--recursive` for the standalone build**; use `checkout_granite.sh`. Never edit files inside `Granite/`.
- **IMPORTANT: do not hand-edit `shaders/slangmosh*.hpp`**: change the shader sources and regenerate with `slangmosh.sh`.
- **The C API in `pyrowave.h` is shared by Helios and Selene**: a signature or behaviour change MUST be matched in both consumers (and bumped there) in the same release cycle.
- Branches: this repo has no `develop`. Branch from `master` (`feat/*`, `fix/*`, `perf/*`), PR to `master`. Never commit directly to `master`.
- Performance changes count only with a real measurement (encode time on the host, decode time and transit on the device); say so when a gain is not measurable.
- No device-specific code paths or constants taken from the bench devices.

## 4. Workflow preferences

- Change only what the task needs; unrelated findings become a new issue in `eumarcodev/Helios-Server` (component label `codec`, feature label `pyrowave`).
- There is no CI here: build and run `pyrowave-c-test` locally before opening the PR.
- Flow for a codec change: PR here → merge → separate submodule bump PR in Helios and/or Selene.
- Conventional Commits with a scope (`feat(encoder):`, `fix(decoder):`, `perf(c-api):`), one commit per logical phase.

## 5. Out of scope

Do not change without being asked:
- **`Granite/`** (pinned submodule): never edit, clone or bump it on your own.
- **Generated shader headers** `shaders/slangmosh*.hpp` (regenerate only).
- **`LICENSE`**.
- **The user's uncommitted state** in this checkout (e.g. `build/`).
