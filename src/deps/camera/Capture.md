# CAPTURE -- live camera devices

`CAPTURE` is the engine-wide live-video source. It enumerates OS camera
devices, can hold several open at once, and hands the latest frame out as
straight-alpha RGBA8 (top-down). It is named apart from the 3D view `CAMERA`
on `VIEWPORT`. All classes live in namespace `SNEEZE::DEP`.

One instance per `ENGINE`, created after `XR_RUNTIME` and before `UI_CONTEXT`,
reachable via `ENGINE::Capture()`.

This is not Ocean, OpenCV, or SDL. Each OS uses its native capture API so the
engine stays free of those dependencies (and of SDL3, which the host owns).

## Platform backends

| OS | Backend | Notes |
|----|---------|-------|
| Windows | Media Foundation (`IMFSourceReader`) | Prefers RGB32, falls back to NV12 / YUY2 |
| macOS / iOS | AVFoundation | 32BGRA sample buffers |
| Linux | V4L2 | Prefers YUYV 640x480, then NV12 / RGB24 |
| Android | Camera2 NDK (`AImageReader`) | YUV_420_888. Covers Quest 3 as well |

Quest 3 runs the Android backend. RGB camera access on Horizon OS is
permission-gated and may enumerate zero devices until the host requests the
platform camera permission. Headset passthrough is a separate OpenXR path and
is not a `CAPTURE` device.

The host still has to declare OS camera permissions (Windows privacy prompt,
iOS `NSCameraUsageDescription`, Android `CAMERA`, macOS camera entitlement).
The engine cannot do that from a static library.

## API

```cpp
CAPTURE capture (pEngine);
capture.Initialize ();

int nCount = capture.Device_Count ();
std::string sName = capture.Device_Name (0);

if (capture.Device_Open (0))
{
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

`Device_Open` is refcounted: two RmlUi `<img>` tags on `camera://0` share one
device. `Frame_Latest` returns false until the first sample arrives (permission
pending, device warming up). `nFrameIx` increments per new sample so a consumer
can skip redraws.

## RmlUi

`UI_RENDER::LoadTexture` treats `camera://N` as camera device N. Each compositor
pass, `UI_PANEL::Render` calls `UI_RENDER::LiveTexture_Update`, which copies any
new frame into the RmlUi texture (premultiplied) and dirties the panel so it
re-rasters. Until the first sample, `Render` returns false so the compositor
does not GPU-upload the placeholder.

```
<img src="camera://0" style="width: 100%; height: 100%;" />
```

`camera://1` is the second device, and so on. Opening fails (broken image) when
the index is out of range or the OS denies access.

## Demo fabric

`tools/assets/msf/camera.json` is a panel fabric for this path. It loads
`wasm/panel.wasm` (same module as `panel.json`) and puts an RmlUi document in
`Data.sPanel` whose `img src='camera://0'` is the live feed. Serve the
`tools/assets/msf/` folder over HTTP and open `camera.json` in the host
(relative `wasm/panel.wasm` resolves next to the fabric). The host must still
declare OS camera permission.

`UI_RENDER::UpdateTexture` is the same pixel-replace path used by tests and by
any host that wants to push a buffer into an existing RmlUi texture without
going through `CAPTURE`.

## Files

| File | Contents |
|------|----------|
| `Capture.h/.cpp` | CAPTURE -- enumerate / open / latest-frame, refcount |
| `Capture_Platform.h` | Internal backend contract |
| `Capture_Convert.cpp` | BGRA / RGB / YUY2 / NV12 / YUV420 -> RGBA8 |
| `Capture_Win.cpp` | Media Foundation |
| `Capture_Apple.mm` | AVFoundation (ARC) |
| `Capture_Linux.cpp` | V4L2 |
| `Capture_Android.cpp` | Camera2 NDK |
