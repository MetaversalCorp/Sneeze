# Capture

Portable live-camera library. Enumerates OS cameras, opens one or more at a
time, and returns the latest frame as straight-alpha RGBA8 (top-down).

This tree is a standalone CMake project. It currently lives in the Sneeze
repo so it can be extracted later to its own git repository and pinned from
`deps/dependencies.json`. Sneeze consumes it through a thin engine shim
(`src/deps/camera`) that owns lifetime and routes logs to `ENGINE::Log`.

This is not OpenCV, SDL, or Ocean. Each OS uses its native capture API.

## Platforms

| OS | Backend | Notes |
|----|---------|-------|
| Windows | Media Foundation (`IMFSourceReader`) | Prefers RGB32, falls back to NV12 / YUY2 |
| macOS / iOS | AVFoundation | 32BGRA sample buffers |
| Linux | V4L2 | Prefers YUYV 640x480, then NV12 / RGB24 |
| Android | Camera2 NDK (`AImageReader`) | YUV_420_888. Covers Quest 3 as well |

Quest 3 and Quest 3S run the Android backend. Passthrough cameras appear in
Camera2 only after the host holds both `android.permission.CAMERA` and
`horizonos.permission.HEADSET_CAMERA`. `Device_Intrinsics` fills the pinhole
6-tuple (fx, fy, cx, cy, size, EXIF orientation) when the OS supplies
calibration. OpenXR environment blend is a separate path and is not a Capture
device.

The embedding application still declares OS camera permissions (Windows
privacy prompt, iOS `NSCameraUsageDescription`, Android `CAMERA` /
`HEADSET_CAMERA`, macOS camera entitlement). This library cannot do that.

## API

```cpp
#include <Capture/Capture.h>

Capture::CAPTURE capture;
capture.Initialize ();

int nCount = capture.Device_Count ();
std::string sName = capture.Device_Name (0);

if (capture.Device_Open (0))
{
   Capture::CAPTURE::INTRINSICS Intrinsics;
   capture.Device_Intrinsics (0, Intrinsics);

   int nWidth = 0, nHeight = 0;
   uint64_t nFrameIx = 0;
   std::vector<uint8_t> aRgba;
   if (capture.Frame_Latest (0, nWidth, nHeight, aRgba, nFrameIx))
   {
      // straight RGBA8, top-down, A=255
   }
   capture.Device_Close (0);
}
```

`Device_Open` is refcounted. `Frame_Latest` returns false until the first
sample arrives. `nFrameIx` increments per new sample.

Optional logging:

```cpp
void OnLog (Capture::eLOG eLevel, const char* szMessage, void* pUser);
Capture::CAPTURE capture (OnLog, pUser);
```

## Build

```
cmake -S capture -B build -DCMAKE_INSTALL_PREFIX=install
cmake --build build --config Release
cmake --install build --config Release
```

When nested under Sneeze, `src/CMakeLists.txt` `add_subdirectory`s this
project until Capture is a cloned dep under `deps/repos/`.

## Extracting to its own repo

1. Move this `capture/` directory to a new git repository.
2. Add `capture` to `deps/dependencies.json` (url, folder `Capture`, ref).
3. Include `deps/capture.cmake` from `deps/CMakeLists.txt` (`SNEEZE_DEPS`).
4. Switch Sneeze from `add_subdirectory` to `find_package (Capture)` via
   `src/cmake/FindCapture.cmake`.
