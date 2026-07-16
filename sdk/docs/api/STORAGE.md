# STORAGE

Persistent JSON document storage for a module, reached through [`FABRIC::Storage`](FABRIC.md#storage). Use it to save state between sessions - settings, progress, cached results. `STORAGE` is a zero-cost view over the fabric handle.

Values cross the boundary as **JSON text** in both directions. The SDK does not impose a JSON library: `Set` takes a JSON string you produced, `Get` returns a JSON string you parse. Pick whatever JSON facility you like (or none, for plain scalars).

## Scope

Every call takes an `eSNEEZE_ABI_SILO_SCOPE`, selecting which of four storage units the operation targets:

| Scope | Shared by | Lifetime |
|-------|-----------|----------|
| `kSNEEZE_ABI_SILO_SCOPE_PERMANENT_ORG` | all containers of the organization | persists |
| `kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER` | this container only | persists |
| `kSNEEZE_ABI_SILO_SCOPE_TEMPORARY_ORG` | all containers of the organization | session |
| `kSNEEZE_ABI_SILO_SCOPE_TEMPORARY_CONTAINER` | this container only | session |

"Org" scopes are visible to every container under the same organization; the "container" scopes are private to this one. "Permanent" survives restarts; "temporary" is cleared with the session.

## Paths

A path addresses a value inside the scope's document using dotted segments (`"user.profile.name"`). An **empty path** (`""`) addresses the whole root document.

## Methods

### Has

```rust
pub fn Has (&self, eScope: eSNEEZE_ABI_SILO_SCOPE, sPath: &str) -> bool
```

- **Parameters:**
  - `eScope` - which storage unit (see [Scope](#scope)).
  - `sPath` - the value's path (`""` = the whole document).
- **Returns:** `true` if a value exists at `sPath`.
- **Description:** Tests for the presence of a value without reading it.
- **Example:**

```rust
use eSNEEZE_ABI_SILO_SCOPE::*;
if pFabric.Storage ().Has (kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER, "settings")
{
   // restore from settings
}
```

- **See also:** [`Get`](#get).

### Get

```rust
pub fn Get (&self, eScope: eSNEEZE_ABI_SILO_SCOPE, sPath: &str) -> Option<String>
```

- **Parameters:**
  - `eScope` - which storage unit.
  - `sPath` - the value's path (`""` = the whole document).
- **Returns:** `Some(json)` with the value as JSON text, or `None` if missing or null.
- **Description:** Reads the value at `sPath` as a JSON string. The SDK sizes the result buffer in one probe and, if the value is larger than the initial buffer, one exact re-read - so a large value is returned correctly without you managing buffers. Parse the returned string with your JSON library of choice.
- **Example:**

```rust
use eSNEEZE_ABI_SILO_SCOPE::*;
if let Some (sJson) = pFabric.Storage ().Get (kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER, "settings")
{
   // parse sJson with your JSON crate
}
```

- **See also:** [`Has`](#has), [`Set`](#set), [`DATA::Get`](DATA.md#get) (the read-only analog).

### Set

```rust
pub fn Set (&self, eScope: eSNEEZE_ABI_SILO_SCOPE, sPath: &str, sJson: &str) -> bool
```

- **Parameters:**
  - `eScope` - which storage unit.
  - `sPath` - where to write (`""` = replace the whole document).
  - `sJson` - the value, as JSON text.
- **Returns:** `true` on success.
- **Description:** Writes `sJson` at `sPath`, creating intermediate objects as needed. Writing to the empty path replaces the entire document. The write is durable for "permanent" scopes.
- **Example:**

```rust
use eSNEEZE_ABI_SILO_SCOPE::*;
pFabric.Storage ().Set (kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER, "settings.volume", "0.8");
```

- **See also:** [`Get`](#get), [`Remove`](#remove).

### Remove

```rust
pub fn Remove (&self, eScope: eSNEEZE_ABI_SILO_SCOPE, sPath: &str) -> bool
```

- **Parameters:**
  - `eScope` - which storage unit.
  - `sPath` - what to delete (`""` = clear the whole document).
- **Returns:** `true` on success.
- **Description:** Deletes the value at `sPath`. Removing the empty path clears the entire document.
- **Example:**

```rust
use eSNEEZE_ABI_SILO_SCOPE::*;
pFabric.Storage ().Remove (kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER, "settings.volume");
```

- **See also:** [`Set`](#set).
