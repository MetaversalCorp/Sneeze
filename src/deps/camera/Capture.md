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

Quest 3 and Quest 3S run the Android backend. Passthrough cameras appear in
Camera2 only after the host holds both `android.permission.CAMERA` and
`horizonos.permission.HEADSET_CAMERA` (runtime prompts, Horizon OS v74 or
later). `camera://0` is the left passthrough camera and `camera://1` the
right, when those vendor tags are present; other cameras follow. Capture runs
on a thread with a looper, because Camera2 delivers frames there, and the
reader size is a YUV size the device actually advertises (near 1280x960).
OpenXR environment blend (the AR toggle) is a separate path and is not a
`CAPTURE` device.

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
   CAPTURE::INTRINSICS Intrinsics;
   if (capture.Device_Intrinsics (0, Intrinsics))
   {
      // width, height, fx, fy, cx, cy, orientation, eModel
      // Model_Name -> PINHOLE (COLMAP / OpenVPS / ARCore / Lightship)
      // Orientation_Name -> TOP_LEFT .. LEFT_BOTTOM (placeframe EXIF)
   }

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

`Device_Intrinsics` returns `INTRINSICS` for an open device. The shared
header is the pinhole 6-tuple every VPS query path uses: `width` / `height`,
`fx` / `fy` / `cx` / `cy`. That is placeframe `PinholeCameraConfig`, OpenVPS
COLMAP `cameras.txt` `PINHOLE`, Google ARCore `CameraIntrinsics`, and Niantic
Spatial `XRCameraIntrinsics`. `eModel` is the COLMAP model name
(`Model_Name` emits `PINHOLE`, `OPENCV`, `OPENCV_FISHEYE`); backends today
always write `kMODEL_PINHOLE`. `orientation` is the eight TIFF/EXIF names
placeframe and OpenVPS rotate with. Units are pixels of `Frame_Latest` (the
CPU stream, matching ARCore `getImageIntrinsics`, not a GPU texture crop).
Android Camera2 / Quest 3 PCA fill from `LENS_INTRINSIC_CALIBRATION` (falling
back to focal length in mm over sensor size) and map `SENSOR_ORIENTATION`
onto those EXIF names. Windows reads Media Foundation pinhole blobs when the
driver supplies them. Apple uses the sample intrinsic matrix, else horizontal
FOV. If the OS has no calibration, a square-pixel estimate from the stream
size is used so a host can still post a config.
OPENCV / OPENCV_FISHEYE are reserved enumerants: a later mapping path can
grow `INTRINSICS` with distortion terms without changing `Device_Intrinsics`.

## RmlUi

`UI_RENDER::LoadTexture` treats `camera://N` as camera device N and reports a
1x1 intrinsic size so layout comes from CSS, not capture resolution. Each
compositor pass, `UI_PANEL::Render` calls `UI_RENDER::LiveTexture_Update`, which
copies any new frame into the RmlUi texture (growing the staging buffer to the
real frame size on the first sample). After the first raster, later frames stamp
that texture's RGB only onto pixels the img actually covered (`LiveTexture_Stamp`)
and keep the first raster's alpha, so rounded-rect holes stay holes. Until the
first sample, `Render` returns false so the compositor does not GPU-upload the
placeholder.

```
<img src="camera://0" style="width: 100%; height: 100%;" />
```

`camera://1` is the second device, and so on. Opening fails (broken image) when
the index is out of range or the OS denies access.

## Demo fabric

`tools/assets/msf/camera.json` is a panel fabric for this path. It loads
`wasm/panel.wasm` (same module as `panel.json`) and puts an RmlUi document in
`Data.sPanel` whose `img src='camera://0'` is the live feed. The card uses
`box-sizing: border-box` so its percentage size includes padding and border.
The feed is a normal-flow block (`width: 100%; height: 78%`) inside the card so
the img is not sized from capture resolution. Serve the `tools/assets/msf/`
folder over HTTP and open `camera.json` in the host (relative `wasm/panel.wasm`
resolves next to the fabric). The host must still declare OS camera permission.

`UI_RENDER::UpdateTexture` is the same pixel-replace path used by tests and by
any host that wants to push a buffer into an existing RmlUi texture without
going through `CAPTURE`.

## Files

| File | Contents |
|------|----------|
| `Capture.h/.cpp` | CAPTURE -- enumerate / open / latest-frame / pinhole, refcount |
| `Capture_Platform.h` | Internal backend contract |
| `Capture_Pinhole.cpp` | Shared pinhole scale, EXIF orientation, size/FOV/mm fallbacks |
| `Capture_Convert.cpp` | BGRA / RGB / YUY2 / NV12 / YUV420 -> RGBA8 |
| `Capture_Win.cpp` | Media Foundation |
| `Capture_Apple.mm` | AVFoundation (ARC) |
| `Capture_Linux.cpp` | V4L2 |
| `Capture_Android.cpp` | Camera2 NDK |
