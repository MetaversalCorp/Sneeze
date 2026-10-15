# VPS

Engine visual-positioning service. One `VPS` per `ENGINE`, created after `CAPTURE` is up and destroyed after `CONTROL` so a compositor tick cannot overlap teardown. `ENGINE::Vps()` reaches it.

Sneeze does not speak a vendor protocol. `VPS::Tick` reads a `CAPTURE` frame and its pinhole intrinsics, asks the current `IVPS_DEVICE` to localize, and on a fix writes `VIEWPORT::Camera` (Z-up, identity looks along +X). It turns viewport passthrough on so the camera feed shows through, unless the user has switched to VR (`Passthrough_User(false)`); that choice sticks until they turn AR on again.

`Note()` is the last localize result (`"pose"`, an error string, or empty). A panel document that includes `vps-status` (and optionally `vps-map`, `vps-pos`, `vps-rot`, `vps-conf`) is filled from that each frame. The panel is an ordinary scene node: `Transform.Position` places it. `tools/assets/msf/camera3.json` is that fabric.

The idle device is a stub. A Placeframe device is installed when a service is configured. It `POST`s multipart `/localize` (map ids, `PinholeCameraConfig`, `axis_convention=OPENCV`, a BMP of the frame) with an optional bearer token, and converts `camera_from_map_transform` into the Sneeze pose. The localizer itself stays a remote service.

Localization only works when the camera frame matches the orientation of the camera that scanned the map. The frame sent is the sensor buffer, not rotated to the way the phone is held, with the sensor's fixed orientation tag. A map scanned in portrait and stored as upright images will not match that landscape sensor frame.

Configuration, first match wins for each field that is set:

- `SNEEZE_VPS_URL`, `SNEEZE_VPS_TOKEN`, `SNEEZE_VPS_USER`, `SNEEZE_VPS_PASSWORD`, `SNEEZE_VPS_MAPS` (comma-separated ids), `SNEEZE_VPS_DEVICE`, `SNEEZE_VPS_INTERVAL`
- `<sAppDataPath>/vps.json` with `url`, `token` or `username`/`password`, `maps`, `device`, `interval`
- A URL ending in `vps.json` typed in the address bar (for example `https://cdn.rp1.com/fabric/vps.json`). That download replaces the running config and does not open a fabric.

A username and password log in through Keycloak (`client_id` `placeframe-api`, realm `placeframe-dev`) and the access token is sent as a bearer. `GET /localization-maps` fills the catalog (`id` and `name`). Localization does not start until `Map_Select` names one map. On Android the host shows that catalog as a picker. **Skip** calls `Skip()`: no camera read and no pose write until a map is chosen again. With no map selected, the phone's rotation drives the AR view.

An empty URL leaves the stub installed and `IsRunning()` false. Queries run off the compositor thread. A query already in flight is left alone until `dInterval` seconds after it was started (minimum 0.5).
