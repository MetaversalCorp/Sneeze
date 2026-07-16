# CONSOLE

Developer-console logging for a fabric, reached through [`FABRIC::Console`](FABRIC.md#console). Lines are forwarded to the container's log stream, where the host application surfaces them (a `console.*` analog). `CONSOLE` is a zero-cost view over the fabric handle.

Most methods take a single message string and differ only in severity or grouping behavior. They all share the same shape, so the per-method entries below are brief; the two that differ (`Assert`, `Group_End`) are called out.

## Severity methods

Each of these logs one line at the named level:

```rust
pub fn Log   (&self, sText: &str)
pub fn Debug (&self, sText: &str)
pub fn Info  (&self, sText: &str)
pub fn Warn  (&self, sText: &str)
pub fn Error (&self, sText: &str)
```

- **Parameters:** `sText` - the message.
- **Returns:** nothing.
- **Description:** Emit `sText` at the level named by the method. `Log` is the default level; `Debug`/`Info`/`Warn`/`Error` set severity, which the host may filter or color.
- **Example:**

```rust
let pConsole = pFabric.Console ();
pConsole.Log   ("scene built");
pConsole.Warn  ("texture missing, using fallback");
pConsole.Error ("failed to parse data");
```

- **See also:** [`Assert`](#assert), [`Group`](#group-group_collapsed-group_end).

## Assert

```rust
pub fn Assert (&self, bCondition: bool, sText: &str)
```

- **Parameters:**
  - `bCondition` - the assertion; when `false`, the message is logged.
  - `sText` - the message to log if `bCondition` is `false`.
- **Returns:** nothing.
- **Description:** Logs `sText` only when `bCondition` is `false`. A no-op when the condition holds. Mirrors `console.assert`.
- **Example:**

```rust
pFabric.Console ().Assert (pFabric.Modules ().len () > 0, "manifest has no modules");
```

- **See also:** [`Error`](#severity-methods).

## Group, Group_Collapsed, Group_End

```rust
pub fn Group           (&self, sText: &str)
pub fn Group_Collapsed (&self, sText: &str)
pub fn Group_End       (&self)
```

- **Parameters:**
  - `Group` / `Group_Collapsed`: `sText` - the group label.
  - `Group_End`: none.
- **Returns:** nothing.
- **Description:** Open a nested, indented group of log lines. `Group` opens it expanded; `Group_Collapsed` opens it collapsed. `Group_End` closes the most recently opened group. Mirrors `console.group` / `console.groupCollapsed` / `console.groupEnd`.
- **Example:**

```rust
let pConsole = pFabric.Console ();
pConsole.Group ("loading assets");
pConsole.Log ("Stool.glb");
pConsole.Log ("Bucket.glb");
pConsole.Group_End ();
```

- **See also:** [`Log`](#severity-methods).

## Count, Count_Reset

```rust
pub fn Count       (&self, sText: &str)
pub fn Count_Reset (&self, sText: &str)
```

- **Parameters:** `sText` - the counter's label.
- **Returns:** nothing.
- **Description:** `Count` increments a named counter and logs its running total; `Count_Reset` sets it back to zero. Mirrors `console.count` / `console.countReset`.
- **Example:**

```rust
pFabric.Console ().Count ("nodes-created");
```

- **See also:** [`Time`](#time-time_end-time_log).

## Time, Time_End, Time_Log

```rust
pub fn Time     (&self, sText: &str)
pub fn Time_End (&self, sText: &str)
pub fn Time_Log (&self, sText: &str)
```

- **Parameters:** `sText` - the timer's label.
- **Returns:** nothing.
- **Description:** `Time` starts a named timer; `Time_Log` logs its elapsed time without stopping it; `Time_End` stops it and logs the final elapsed time. Mirrors `console.time` / `console.timeLog` / `console.timeEnd`.
- **Example:**

```rust
let pConsole = pFabric.Console ();
pConsole.Time ("build");
// ... build the scene ...
pConsole.Time_End ("build");
```

- **See also:** [`Count`](#count-count_reset).
