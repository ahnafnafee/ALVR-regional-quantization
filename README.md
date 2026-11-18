# Keeping up with the ride

**Code companion for _Keeping up with the ride: Motion-aware compression for wireless VR cycling_.** This fork of ALVR v20.14.1 adds game-informed regional quantization to the Windows NVIDIA NVENC HEVC encoder. A VR application supplies the positions of objects and surfaces in each eye. ALVR transforms those regions into fixed-foveated encoder coordinates and uses them to request finer or coarser quantization by block.

In the Pedaltopia study, the two encodings shared a 16 Mbps video target. Across 288 matched moving replay frames, regional encoding used 1.51% fewer encoded bits and reduced mean squared color error by 15.20–16.25% in the road, cockpit, and tagged hazard regions across both eyes. The five-rider study evaluated the two encodings under a shared 21 Mbps Wi-Fi traffic cap.

## Reproduce the encoder changes

The paper points to [`cs692-paper-v1.1`](https://github.com/ahnafnafee/ALVR-regional-quantization/tree/cs692-paper-v1.1). Use that tag for a fixed checkout. The `cs692-paper` branch is the maintained entry point.

```powershell
git clone --branch cs692-paper-v1.1 https://github.com/ahnafnafee/ALVR-regional-quantization.git
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

## Game-hook API

The game supplies a snapshot of visible geometry and tracking state. The C++
publisher projects it into left- and right-eye rectangles and sends it to
ALVR over Windows shared memory. Build `AttentionWorker.cpp` into the
application or its native plugin using C++17 and the producer and ALVR
Windows include directories. Use `AttentionWorker.h` for the C API. Start
the publisher inside the active SteamVR scene process. [Publisher header](tools/regional-quantization/producer/AttentionWorker.h)

| Call | When to use it |
| --- | --- |
| `FBA_Create(sizeof(Frame), sizeof(Shape), MappingName, MutexName)` | Open the channel once at scene startup after the regional ALVR driver is active. A null handle means creation failed. |
| `FBA_Submit(handle, &frame, shapes)` | Submit the scene snapshot before ALVR captures the rendered frame. A return value of `1` means the worker queued a copy. The worker processes the most recent request asynchronously. |
| `FBA_Stats(handle, &stats)` | Inspect published snapshots and counts for superseded, expired, or busy submissions. |
| `FBA_Destroy(handle)` | Shut down the worker when the scene closes. |

Set `Frame.bytes` to `sizeof(Frame)` and `Frame.abi` to `scene::Abi`.
Supply a frame counter, performance-counter ticks and frequency, a monotonic
time in seconds, the tracked and world-space head orientations, the head
position, a positive near plane, and the two column-major view-projection
matrices in left-right order. Set the `Active` flag for regional publishing.
`Selective` enables distance-based priority. `Predictive` lets the publisher
use the configured head-turn horizon when selecting focus regions. The weights
in `Frame.settings` express relative quantization priority. [Frame and shape types](tools/regional-quantization/producer/AttentionTypes.h)

Pass an array of `Frame.count` shapes, up to `MaxShapes` (128). Each shape
holds up to eight world-space points. The study uses semantic IDs `1` for the
road, `2` for the cockpit, `3` for a nearby rider, and `4` for a hazard.
Set `Shape.projection` to `0` for a bounding rectangle or `1` to project a
convex quad into horizontal bands. `distance`, `fullUntil`, `fadeUntil` and
`minimum` define priority falloff when `Selective` is enabled. [Runnable synthetic bridge example](tools/regional-quantization/tests/worker_bridge_tests.cpp)

Applications that already project their own rectangles can also use the
`AttentionPacket.h` wire format. Rectangles use normalized source-eye
coordinates with a top-left origin. ALVR checks scene ownership, packet age,
and the frame request before building the encoder map. Missing or stale
regions fall back to the fixed-foveated encoding. While streaming, run
`python tools/regional-quantization/read_status.py --seconds 10` and check that
`scene_pid` matches the application and `applied` advances. [Wire format and shared-memory names](alvr/server_openvr/cpp/platform/win32/AttentionPacket.h)

## Release scope

The public artifact covers the ALVR modifications, reusable C++ hook, and standalone build and test tools. Pedaltopia's game source and assets remain proprietary, and the study recordings remain private. The synthetic tests exercise the regional encoding path with generated inputs. Repeating the rider study requires the original application and study setup. The public source uses the [MIT license](LICENSE). [Upstream ALVR v20.14.1](https://github.com/alvr-org/ALVR/tree/v20.14.1)

## Cite the study

Ahnaf An Nafee. 2026. _Keeping up with the ride: Motion-aware compression for wireless VR cycling_. CS 692 final project report, George Mason University.


```bibtex
@techreport{nafee2026keeping,
  author      = {Nafee, Ahnaf An},
  title       = {Keeping Up with the Ride: Motion-Aware Compression for Wireless {VR} Cycling},
  institution = {George Mason University},
  type        = {CS 692 final project report},
  year        = {2026}
}
```

For the implementation, reference the [`cs692-paper-v1.1` source snapshot](https://github.com/ahnafnafee/ALVR-regional-quantization/tree/cs692-paper-v1.1).
