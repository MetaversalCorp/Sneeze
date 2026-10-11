# XR — OpenXR runtime, face & body tracking

The `xr` module probes for an OpenXR runtime and (on Android XR / Galaxy XR)
drives **face** (`XR_ANDROID_face_tracking`) and **body**
(`XR_ANDROIDSYS_body_tracking` / aliases) tracking into openxr-free PODs
consumed by Space-Time Host and OpenNexus.

## Public types

[`include/XrTracking.h`](../../../include/XrTracking.h) — `XR_FACE_STATE` (68 params),
`XR_BODY_STATE` (14 upper-body joints), JSON helpers matching
OpenNexus `openxrFaceParameterMap.js` / `nativeFaceBridge.js`.

## XR_RUNTIME

```cpp
DEP::XR_RUNTIME* xr = engine.Xr ();
xr->Initialize (); // also RefreshExtensionProbe

if (xr->HasRuntime ())
   auto name = xr->GetRuntimeName ();

auto caps = xr->Capabilities ();
// caps.bExtensionFaceAndroid / bExtensionBodyAndroidSys / …

// DGX / CI without a headset:
XR_FACE_STATE face; face.bValid = true; face.aParameters[24] = 0.5f; // jaw_drop
xr->InjectFaceFixture (face);

XR_FACE_STATE live;
if (xr->PollFace (live)) { /* … */ }

// Galaxy XR (from JNI host after FACE_TRACKING + BODY_TRACKING granted):
xr->BeginAndroidSession (javaVM, activity, nativeWindow);
xr->PumpAndroidTracking ();
xr->EndAndroidSession ();
```

`ENGINE` wrappers: `XrCapabilities`, `XrPollFace`, `XrPollBody`,
`XrInjectFaceFixture`, `XrInjectBodyFixture`, `XrSetAvatarBindId`.

## Permissions (Android XR / Galaxy XR)

| Capability | Extension | Permission |
|------------|-----------|------------|
| Face blendshapes | `XR_ANDROID_face_tracking` | `android.permission.FACE_TRACKING` |
| Upper body IK | `XR_ANDROIDSYS_body_tracking` (also try `XR_ANDROIDX_body_tracking` / `XR_ANDROID_body_tracking`) | `android.permission.BODY_TRACKING` |
| Multimodal face | `XR_ANDROID_face_tracking_data_source` | + `RECORD_AUDIO` when audio source |

## Related projects

| Project | Role |
|---------|------|
| Space-Time Host | `--xr-probe`, `--xr-fixture-demo`, `--xr-relay` |
| OpenNexus `native/android-xr-face-bridge` | APK OpenXR face (+ body) → WebView / HTTP relay |
| OpenNexus `openxrFaceParameterMap.js` | Same 68 WebXR keys |

## Files

| File | Contents |
|------|----------|
| `XrRuntime.h` | XR_RUNTIME API |
| `XrRuntime.cpp` | Instance + extension probe + fixtures |
| `XrRuntime_Android.cpp` | Android session / face / body (stubs off-Android) |
| `XrRuntimeImpl.h` | Shared impl |
| `XrFaceParameterMap.cpp` | Key tables + JSON |
| `XrRuntime_Stub.cpp` | No-OpenXR builds |
