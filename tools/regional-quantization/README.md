# Regional quantization for ALVR 20.14.1

This Windows server fork uses an application's scene data guide video
quantization. It was developed for the Pedaltopia wireless VR cycling study.
The application supplies regions separately for each eye while SteamVR runs the scene. ALVR maps those regions
through its fixed-foveation transform, computes block-level delta QPs and sends
the map to NVIDIA NVENC in HEVC mode. This changes how the encoder distributes
precision within the stream. The headset client uses the standard ALVR protocol.

The fork starts from upstream [ALVR v20.14.1](https://github.com/alvr-org/ALVR/releases/tag/v20.14.1)
at commit `a9f6542fa507a841f40ab4f3fcb531427cd02550`. The additions are available
under the repository's [MIT license](../../LICENSE). Existing upstream and
third-party notices remain in their original locations.

## Build the Windows server driver

Use a Windows x64 checkout, Git, Python 3.10 or later, Rust for the
`x86_64-pc-windows-msvc` target, and Visual Studio with the x64 C++ build tools and
Windows SDK. From the checkout root, run:

```powershell
python tools/regional-quantization/build_driver.py --cache C:/Temp/alvr-regional-build
```

The script builds the committed checkout and downloads pinned dependencies into
the cache. It uses the study's Cargo lock for this driver build. The result is
`C:/Temp/alvr-regional-build/target/release/alvr_server_openvr.dll`.

Use the matching upstream ALVR 20.14.1 server and headset client. Close ALVR and
SteamVR, back up `bin/win64/driver_alvr_server.dll` under the server install, and
replace it with the built DLL using the installed filename. Create an empty
`attention.enabled` file in the ALVR installation root, then restart the server
and SteamVR. Select HEVC with NVIDIA NVENC. For a baseline using the same driver
and settings, close the server and remove the marker before restarting. To restore
the upstream installation, put back the saved DLL and remove the marker.

## Connect an application

The [packet header](../../alvr/server_openvr/cpp/platform/win32/AttentionPacket.h)
defines the wire layout. Send normalized rectangles for each eye before ALVR
captures the frame for composition. The top-left corner is `(0,0)`, with `u`
growing to the right and `v` growing downward. Set the low byte of `Region.kind`
to 1 for road, 2 for cockpit, 3 for a nearby rider, or 4 for a hazard. Bit 8 opts
into an importance value encoded as unsigned normalized 16-bit data in the upper
half of the field. See [the map implementation](../../alvr/server_openvr/cpp/platform/win32/AttentionMap.h)
for how the study policy treats those classes.

The ALVR driver opens the session-local mapping `Local\GVL.StreamingAttention.v1`
and mutex `Local\GVL.StreamingAttention.v1.Lock`. It accepts a fresh packet from
the active scene process. Publish from inside that process. The bundled
[C++ publisher](producer/AttentionWorker.h) projects generic geometry given the
application's column-major stereo view-projection matrices. It also handles locking, timestamps and the
asynchronous request fence. Compile `producer/AttentionWorker.cpp` and include
both `producer` and `alvr/server_openvr/cpp/platform/win32`. The
[worker bridge test](tests/worker_bridge_tests.cpp) demonstrates submitting a
synthetic shape. A standalone program can run that test without starting SteamVR.
A streaming application supplies its own geometry and camera state.

Use the read-only status tool while streaming to confirm that the active scene
PID matches the application and that `applied` advances:

```powershell
python tools/regional-quantization/read_status.py --seconds 10
```

The optional controller in the bridge defaults to disabled. Image capture and
encoder telemetry are opt-in and produce local files. See the source under
`alvr/server_openvr/cpp/platform/win32` for the encoder's capture and telemetry
settings. This repository contains the ALVR changes, the reusable publisher,
and synthetic tests. Pedaltopia's project, assets and study recordings are
separate.

## Run the native tests

Stop any active ALVR stream, then run from the checkout root:

```powershell
python tools/regional-quantization/build_tests.py C:/Temp/alvr-regional-tests
```

The test runner builds the native map and worker/bridge tests, controller tests,
and the capture and telemetry tests. It also builds a separate synthetic NVENC
probe. The probe needs a compatible GPU and does not run as part of the suite.

To smoke-test NVENC with generated images, encode 24 frames with each setting.
The last argument before the options selects whether to send a regional map.

```powershell
$probe = 'C:/Temp/alvr-regional-tests/nvenc_probe.exe'
& $probe C:/Temp/alvr-regional-tests/baseline.hevc 16 0 --study-preset --frames 24
& $probe C:/Temp/alvr-regional-tests/regional.hevc 16 1 --study-preset --frames 24
```

The probe reports the actual encoded rate and requested QP range. Its synthetic
scene exercises the GPU interface. A live stream also needs an application
publisher and a connected headset.
