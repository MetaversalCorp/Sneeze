# LOCATION

A URL split into its parts - a `window.location` analog. You get the fabric's own address from [`FABRIC::Location`](FABRIC.md#snapshot-views), which builds a `LOCATION` from the launching [`RESOURCE`](RESOURCE.md)'s reference (the fabric URL). Because it is a general URL parser, you can also construct one directly from any URL with [`New`](#new), the way the web's `URL` class works.

The parts are read-only: there are no setter methods, only the getters below.

## Methods

### New

```rust
pub fn New (sUrl: &str) -> Self
```

- **Parameters:** `sUrl` - any URL to split.
- **Returns:** a `LOCATION` with the parts filled in.
- **Description:** Splits a URL into protocol / host / pathname. `FABRIC::Location` calls this for you against the fabric's reference, but it is public so you can reuse the same splitting on any URL. For a URL with no scheme, protocol and host are empty and the whole string is treated as the pathname.
- **Example:**

```rust
let pLoc = LOCATION::New ("https://cdn.rp1.com/sneeze/examples/stool.json");
pFabric.Console ().Log (pLoc.Host ());       // cdn.rp1.com
pFabric.Console ().Log (pLoc.Pathname ());   // /sneeze/examples/stool.json
```

### Href

```rust
pub fn Href (&self) -> &str
```

- **Returns:** the full URL, e.g. `https://cdn.rp1.com/sneeze/examples/stool.json`.

### Protocol

```rust
pub fn Protocol (&self) -> &str
```

- **Returns:** the scheme with its colon, e.g. `https:` (empty when the URL has no scheme).

### Host

```rust
pub fn Host (&self) -> &str
```

- **Returns:** the host (and port, if present), e.g. `cdn.rp1.com` (empty when the URL has no scheme).

### Pathname

```rust
pub fn Pathname (&self) -> &str
```

- **Returns:** the path portion, e.g. `/sneeze/examples/stool.json`.

### Origin

```rust
pub fn Origin (&self) -> String
```

- **Returns:** scheme + host, e.g. `https://cdn.rp1.com`. Empty when the URL has no scheme.

## Usage

```rust
fn Open (pFabric: FABRIC)
{
   let pLoc = pFabric.Location ();

   pFabric.Console ().Log (pLoc.Href ());
   pFabric.Console ().Log (&pLoc.Origin ());
}
```

## See also

- [FABRIC](FABRIC.md#snapshot-views) - `Location ()` builds this from the fabric URL.
- [RESOURCE](RESOURCE.md) - the reference `FABRIC::Location` is derived from.
