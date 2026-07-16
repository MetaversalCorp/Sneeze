# SNEEZE_ABI_MAPOBJECT

The fluent builder for a scene object. You fill one in guest memory, then hand it to [`SCENE::Node_Root`](SCENE.md#node_root) or [`SCENE::Node_Open`](SCENE.md#node_open) to create a [`NODE`](NODE.md). It is a 528-byte binary wire struct whose layout mirrors the engine's `MAP_OBJECT` field for field; a compile-time assert guards its size.

Unlike everything else in the SDK, a map object crosses the boundary as **raw bytes**, not a serialized packet - the create call passes its `(offset, length)`. The builder setters return `&mut Self`, so calls chain.

## Construction

Create one with a class factory, which stamps the object's class and asks the engine to assign the next free per-container index (so you rarely set the index by hand). Sensible defaults are applied: identity rotation and unit scale.

### Class factories

```rust
pub fn Root        () -> Self   // class 70
pub fn Celestial   () -> Self   // class 71
pub fn Terrestrial () -> Self   // class 72
pub fn Physical    () -> Self   // class 73
pub fn Panel       () -> Self   // class 74
pub fn Light       () -> Self   // class 75
pub fn New (wClass: u16) -> Self // explicit class id
```

- **Returns:** a new builder stamped with the class.
- **Description:** Pick the factory for the kind of object you are creating. `New` takes a raw `kSNEEZE_ABI_MAP_OBJECT_CLASS_*` id if you need it. The self index starts as the "assign me the next free index" sentinel; override with [`Index`](#index).
- **Example:**

```rust
let mut pObj = SNEEZE_ABI_MAPOBJECT::Physical ();
```

- **See also:** [`Index`](#index), [`SCENE`](SCENE.md).

## Identity

### Parent

```rust
pub fn Parent (&mut self, wClass: u16, twObjectIx: u64) -> &mut Self
```

- **Parameters:** `wClass` - the parent's class; `twObjectIx` - the parent's object index.
- **Returns:** `&mut Self` (chainable).
- **Description:** Sets the parent by composing the parent's class and object index into the object's Head. This is how [`Scene::Node_Open`](SCENE.md#node_open) learns where to attach the new node - it reads the composed parent straight out of the Head. Name any parent object index you like; when you have the parent as a live `NODE`, pass `pParent.Class (), pParent.ObjectIx ()`.
- **See also:** [`ObjectIx`](#objectix), [`SCENE::Node_Open`](SCENE.md#node_open).

### ObjectIx

```rust
pub fn ObjectIx (&mut self, twObjectIx: u64) -> &mut Self
```

- **Parameters:** `twObjectIx` - the explicit per-container object index.
- **Returns:** `&mut Self` (chainable).
- **Description:** Overrides the auto-assigned object index with a specific one, keeping the class already stamped by the factory. Use only when you deliberately want to name a specific object; otherwise let the engine assign it.
- **Example:**

```rust
let mut pObj = SNEEZE_ABI_MAPOBJECT::Physical ();
pObj.ObjectIx (5);
```

- **See also:** [Class factories](#class-factories).

## Name and resource

### Name

```rust
pub fn Name (&mut self, sName: &str) -> &mut Self
```

- **Parameters:** `sName` - the object's name (stored as UTF-16, up to 48 code units; longer names are truncated).
- **Returns:** `&mut Self` (chainable).
- **Description:** Sets the object's display/identification name.
- **Example:**

```rust
pObj.Name ("Stool");
```

- **See also:** [`NODE::Name`](NODE.md#name) (mutate after creation).

### Reference

```rust
pub fn Reference (&mut self, sReference: &str) -> &mut Self
```

- **Parameters:** `sReference` - the resource URL (stored as UTF-8, up to 127 bytes; longer references are truncated).
- **Returns:** `&mut Self` (chainable).
- **Description:** Sets the object's resource URL - the model or asset (e.g. a GLB) the engine fetches for this node. Absolute or relative to the fabric.
- **Example:**

```rust
pObj.Reference ("assets/Stool.glb");
```

- **See also:** [`NODE::Resource`](NODE.md#resource).

## Type

### Type, Subtype

```rust
pub fn Type    (&mut self, bType: u8)    -> &mut Self
pub fn Subtype (&mut self, bSubtype: u8) -> &mut Self
```

- **Parameters:** `bType` / `bSubtype` - the class-specific type and subtype codes.
- **Returns:** `&mut Self` (chainable).
- **Description:** Sets the class-specific type discriminators (for example, the light type for a `Light ()` object). The meaning is defined per class.
- **See also:** [Class factories](#class-factories).

## Transform

```rust
pub fn Position   (&mut self, dX: f64, dY: f64, dZ: f64)          -> &mut Self
pub fn Rotation   (&mut self, dX: f64, dY: f64, dZ: f64, dW: f64) -> &mut Self
pub fn Scale      (&mut self, dScale: f64)                        -> &mut Self
pub fn Scale_Axes (&mut self, dX: f64, dY: f64, dZ: f64)          -> &mut Self
pub fn Bound      (&mut self, dX: f64, dY: f64, dZ: f64)          -> &mut Self
```

- **Parameters:**
  - `Position` - position in the right-handed Z-up world frame (X east, Y north, Z up).
  - `Rotation` - orientation as a quaternion `(x, y, z, w)`; identity is `(0, 0, 0, 1)`.
  - `Scale` - uniform scale on all axes.
  - `Scale_Axes` - per-axis scale.
  - `Bound` - the three bounding extents.
- **Returns:** `&mut Self` (chainable).
- **Description:** Set the object's initial transform at creation time. Defaults are identity rotation and unit scale. `Scale` and `Scale_Axes` both write the scale; call one.
- **Example:**

```rust
pObj.Position (0.0, 0.0, 0.428).Rotation (0.0, 0.0, 0.0, 1.0).Scale (1.0);
```

- **See also:** the matching live mutators on [`NODE`](NODE.md#position).

## Orbit

### Orbit

```rust
pub fn Orbit (&mut self, dA: f64, dB: f64, tmPeriod: i64, tmOrigin: i64) -> &mut Self
```

- **Parameters:** `dA`, `dB` - the orbit's ellipse parameters; `tmPeriod` - the orbital period; `tmOrigin` - the epoch the orbit is measured from.
- **Returns:** `&mut Self` (chainable).
- **Description:** Sets an object's orbital parameters as a unit (used by celestial objects). All four are set together.
- **See also:** [`Properties_Celestial`](#properties_celestial).

## Properties (set as a unit, one call per class)

Properties are set as a whole for a class, not one field at a time. The color argument is a `0xRRGGBB` value packed into the color field's bits - the engine reads those bits as the color.

### Properties_Celestial

```rust
pub fn Properties_Celestial (&mut self, fMass: f32, fGravity: f32, dwColor: u32, fBrightness: f32, fReflectivity: f32) -> &mut Self
```

- **Parameters:** `fMass`, `fGravity` - physical properties; `dwColor` - `0xRRGGBB`; `fBrightness`, `fReflectivity` - shading properties.
- **Returns:** `&mut Self` (chainable).
- **Description:** Sets all celestial-object properties at once. Use on objects created with `Celestial ()`.
- **Example:**

```rust
let mut pSun = SNEEZE_ABI_MAPOBJECT::Celestial ();
pSun.Name ("Sun").Properties_Celestial (1.0, 1.0, 0xFFDD66, 1.0, 0.0);
```

- **See also:** [`Orbit`](#orbit), [Class factories](#class-factories).

### Properties_Light

```rust
pub fn Properties_Light (&mut self, fOpeningAngle: f32, fFalloffAngle: f32, dwColor: u32, fBrightness: f32) -> &mut Self
```

- **Parameters:** `fOpeningAngle`, `fFalloffAngle` - the spot cone angles, in degrees (spot lights only); `dwColor` - `0xRRGGBB`; `fBrightness` - intensity.
- **Returns:** `&mut Self` (chainable).
- **Description:** Sets all light properties at once. The cone angles overlay the same bytes the celestial properties use, so this is the light-class counterpart of `Properties_Celestial`. Use on objects created with `Light ()`.
- **Example:**

```rust
let mut pLight = SNEEZE_ABI_MAPOBJECT::Light ();
pLight.Type (2)   // spot
      .Properties_Light (53.0, 5.0, 0xFFFFFF, 4.0);
```

- **See also:** [`Type`](#type-subtype), [Class factories](#class-factories).

## Wire access

### Pointer, SIZE

```rust
pub const SIZE: usize = 528;
pub fn Pointer (&self) -> *const u8
```

- **Description:** `SIZE` is the fixed byte length of the struct; `Pointer` returns the address of its bytes. The SDK uses these internally to pass the object to `SCENE`; you rarely call them directly.
- **See also:** [`SCENE::Node_Root`](SCENE.md#node_root).
