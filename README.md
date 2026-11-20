# PyroWave

PyroWave is an intra-only video codec (practically speaking a still-image codec) optimized for extremely fast GPU
encode and decode (< ~0.1 ms at 1080p, < ~0.2 ms at 4K), fully implemented in Vulkan compute shaders. Images are
transformed with a CDF 9/7 discrete wavelet transform (as in JPEG2000), coded with trivial, highly parallel coefficient
coding and an exact single-pass rate control, in independent 64x64 blocks that recover well from packet loss. The
targeted bitrates are high (~200+ Mbit/s): the use case is local network game streaming with the lowest possible
latency. The bitstream is defined in [bitstream/bitstream.md](bitstream/bitstream.md).

This repository is the fork `eumarcodev/pyrowave`, used as a submodule by the
[Helios](https://github.com/eumarcodev/Helios-Server) host (encoder, Windows) and the
[Selene Game Streaming](https://github.com/eumarcodev/Selene-Game-Streaming) Android client (decoder).

## Credits

PyroWave is created by **Hans-Kristian Arntzen (Themaister)**: [Themaister/pyrowave](https://github.com/Themaister/pyrowave).
It is part of his [pyrofling](https://github.com/Themaister/pyrofling) project and is similar in scope to his
[master thesis from 2014](https://ntnuopen.ntnu.no/ntnu-xmlui/handle/11250/2400689). This fork keeps the original
license and copyright notice.

## What changed in this fork

- C API: create a device with extra Vulkan instance and device extensions, so a host can share its own textures (D3D11 in Helios) with the encoder.
- Encoder: stable rate control, so still areas of the picture do not flicker.
- Decoder: block packets smaller than their header are rejected (fixes a decoder hang on corrupt input).
- Performance: per-stage GPU timestamps are opt-in, and wrapped image views are cached in the C API.

## Building

Granite is a git submodule pinned by this repository and only a small portion of it is needed. Do not clone with
`--recursive`, since Granite has many nested submodules that are not needed here.

```shell
bash checkout_granite.sh
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=output -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja install          # installs pyrowave.h, the pyrowave-shared library and its CMake/pkg-config files
./pyrowave-c-test      # unit test of the C API, also a basic guide to it
```

The build is tested on Linux, MinGW, MSYS2 and MSVC; `build-steamrt.sh` builds against the Sniper SDK. For Android,
add `-DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a`.

The sample and CLI tools (`pyrowave-encode`, `pyrowave-decode`) need the full Granite
(`git submodule update --init --recursive Granite`) and `-DPYROWAVE_DEVEL=ON`.

## License

MIT, see [LICENSE](LICENSE).
