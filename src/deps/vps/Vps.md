# VPS

Engine visual-positioning service. One `VPS` per `ENGINE`, created after `CAPTURE` is up and destroyed after `CONTROL` so a compositor tick cannot overlap teardown. `ENGINE::Vps()` reaches it.

Sneeze does not speak a vendor protocol. `VPS::Tick` reads a `CAPTURE` frame and its pinhole intrinsics, asks the current `IVPS_DEVICE` to localize, and applies the fix (Z-up, identity looks along +X). It turns viewport passthrough on so the camera feed shows through, unless the user has switched to VR (`Passthrough_User(false)`); that choice sticks until they turn AR on again.

A fix corrects the tracking frame; it does not replace the tracked camera. `Tick` samples `VIEWPORT::Orientation` with each frame it sends. When the answer arrives, the map pose of the camera times the inverse of that capture orientation gives the heading of the tracking frame in the map (kept to a turn about +Z, since both frames are level with gravity), and the map position gives its origin. That is written as `VIEWPORT::Alignment_Set`, and the compositor composes it with the live orientation every frame, so the view keeps turning with the device and any rotation made while the request was in flight is already accounted for. The first fix snaps; later fixes slew in over 0.5 s with a smoothstep. This mirrors Placeframe's `RelocalizationFilter` without its covariance gating. The phone tracker is rotation-only, so the eye sits at the last fix position until the next one; translation after a capture cannot be compensated until a 6DoF tracker supplies a device position. A frame sent with no orientation (desktop webcam, or a phone's first frame before passthrough starts the tracker) falls back to writing `VIEWPORT::Camera` as an absolute pose and asks again at once, one time only; the next aligned fix releases that pose with `Camera_Deactivate` so the orientation drives the view again. `Skip` or deselecting the map clears the alignment.

`Note()` is the last localize result (`"pose"`, an error string, or empty). A panel document that includes `vps-status` (and optionally `vps-map`, `vps-pos`, `vps-rot`, `vps-conf`) is filled from that each frame. The panel is an ordinary scene node: `Transform.Position` places it. `tools/assets/msf/camera3.json` is that fabric.

The idle device is a stub. A Placeframe device is installed when a service is configured. It `POST`s multipart `/localize` (map ids, `PinholeCameraConfig`, `axis_convention=OPENCV`, a BMP of the frame) with an optional bearer token, and converts `camera_from_map_transform` into the Sneeze pose. The localizer itself stays a remote service.

Localization only works when the camera frame matches the orientation of the camera that scanned the map. The pixels stay in the sensor buffer. The `orientation` field sent with each localize is the live device hold combined with the camera's fixed mount, so a phone held in portrait is tagged differently from the same phone held in landscape.

Configuration, first match wins for each field that is set:

- `SNEEZE_VPS_URL`, `SNEEZE_VPS_TOKEN`, `SNEEZE_VPS_USER`, `SNEEZE_VPS_PASSWORD`, `SNEEZE_VPS_MAPS` (comma-separated ids), `SNEEZE_VPS_DEVICE`, `SNEEZE_VPS_INTERVAL`
- `<sAppDataPath>/vps.json` with `url`, `token` or `username`/`password`, `maps`, `device`, `interval`
- A URL ending in `vps.json` typed in the address bar (for example `https://cdn.rp1.com/fabric/vps.json`). That download replaces the running config and does not open a fabric.

A username and password log in through Keycloak (`client_id` `placeframe-api`, realm `placeframe-dev`) and the access token is sent as a bearer. `GET /localization-maps` fills the catalog (`id` and `name`). Localization does not start until `Map_Select` names one map. On Android the host shows that catalog as a picker. **Skip** calls `Skip()`: no camera read and no pose write until a map is chosen again. The phone's rotation drives the AR view whether or not a map is selected; a selected map adds the alignment on top.

An empty URL leaves the stub installed and `IsRunning()` false. Queries run off the compositor thread. A query already in flight is left alone until `dInterval` seconds after it was started (minimum 0.5).
