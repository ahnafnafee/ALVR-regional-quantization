# Keeping up with the ride

**Code companion for _Keeping up with the ride: Motion-aware compression for wireless VR cycling_.** This fork of ALVR v20.14.1 adds game-informed regional quantization to the Windows NVIDIA NVENC HEVC encoder. A VR application supplies the positions of objects and surfaces in each eye. ALVR transforms those regions into fixed-foveated encoder coordinates and uses them to request finer or coarser quantization by block.

In the Pedaltopia study, the two encodings shared a 16 Mbps video target. Across 288 matched moving replay frames, regional encoding used 1.51% fewer encoded bits and reduced mean squared color error by 15.20–16.25% in the road, cockpit, and tagged hazard regions across both eyes. The five-rider study evaluated the two encodings under a shared 21 Mbps Wi-Fi traffic cap.

## Reproduce the encoder changes

The paper points to [`cs692-paper-v1`](https://github.com/ahnafnafee/ALVR-regional-quantization/tree/cs692-paper-v1). Use that tag for a fixed checkout. The `cs692-paper` branch is the maintained entry point.

```powershell
git clone --branch cs692-paper-v1 https://github.com/ahnafnafee/ALVR-regional-quantization.git
cd ALVR-regional-quantization
python tools/regional-quantization/build_driver.py --cache C:/Temp/alvr-regional-build
python tools/regional-quantization/build_tests.py C:/Temp/alvr-regional-tests
```

The driver build requires Windows x64, Python 3.10+, Rust for Windows MSVC, and the Visual Studio x64 C++ tools with the Windows SDK. Run the native tests with ALVR streaming stopped. The synthetic NVENC probe also requires a compatible NVIDIA GPU. [Install the driver, connect a publisher, and run the GPU smoke test](tools/regional-quantization/README.md)

| Component | Source |
| --- | --- |
| Region packet format | [`AttentionPacket.h`](alvr/server_openvr/cpp/platform/win32/AttentionPacket.h) |
| Per-eye block map and quantization requests | [`AttentionMap.h`](alvr/server_openvr/cpp/platform/win32/AttentionMap.h) |
| NVENC HEVC integration | [`VideoEncoderNVENC.cpp`](alvr/server_openvr/cpp/platform/win32/VideoEncoderNVENC.cpp) |
| Reusable region publisher and synthetic bridge example | [C++ publisher](tools/regional-quantization/producer/AttentionWorker.h) · [Bridge test](tools/regional-quantization/tests/worker_bridge_tests.cpp) |

## Release scope

The public artifact covers the ALVR modifications, reusable C++ hook, and standalone build and test tools. Pedaltopia's game source and assets remain proprietary, and the study recordings remain private. The synthetic tests exercise the regional encoding path with generated inputs. Repeating the rider study requires the original application and study setup. The public source uses the [MIT license](LICENSE). [Upstream ALVR v20.14.1](https://github.com/alvr-org/ALVR/tree/v20.14.1)

## Cite the study

Ahnaf An Nafee. 2026. _Keeping up with the ride: Motion-aware compression for wireless VR cycling_. CS 692 final project report, George Mason University.

For the implementation, reference the [`cs692-paper-v1` source snapshot](https://github.com/ahnafnafee/ALVR-regional-quantization/tree/cs692-paper-v1).
