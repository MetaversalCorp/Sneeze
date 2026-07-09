# Example 01 - A Single Stool

This example will guide you through making a very simple spatial fabric. By the end of it you will have put one 3D object on the screen, and you will understand the handful of parts that every other example is built from.

## What is a spatial fabric?

A spatial fabric is a 3D space described as data. Instead of writing a program that draws a scene, you write a file that *describes* what is in the scene, and the engine reads that file and draws it for you. The file is called a spatial fabric. This example's fabric is `stool.json`, and it describes a scene containing exactly one thing: a stool.

Think of it like a web page. A web page is a text file that describes a document, and the browser turns it into something you can look at. A fabric is a text file that describes a 3D space, and the engine turns it into something you can look at and move through.

## ⚠️ This is not the preferred way to publish

Loading a fabric as plain, unsigned JSON as shown below works today and is convenient while you are learning, but it is not how a fabric is meant to be published, and it may not always work. Browsers will expect fabric files to to be *signed*: signing wraps the fabric with proof of who created it and a guarantee that it was not altered on its way to the browser. Plain JSON offers neither, so the browser treats it as a convenience for local experimentation, not as a real, publishable fabric. A later example introduces signing and shows how to turn a plain fabric like this one into a signed, publishable file; see [Publishing a Signed Fabric](../03-signing/README.md) for the direction that is headed.

## What this example teaches

- What a fabric file looks like and what each part of it is for.
- How a scene is built from nodes, where a node is a single thing in the space.
- How to attach a 3D model (a `.glb` file) to a node so it appears on screen.
- Where the light comes from when you have not added any lights of your own.

## The files

| File | What it is |
|---|---|
| `stool.json` | The fabric. This one file is the entire scene. |
| `wasm/map.wasm` | The stock module that reads the scene from `data.scene` and turns it into a scene. It lives in a shared `wasm` folder because every map-managed example uses the same one. |
| `assets/Stool.glb` | The 3D model of the stool. It lives in a shared `assets` folder because later examples reuse it. |

The model is not stored inside the fabric. The fabric only holds the *address* of the model, and the engine downloads the model from that address when it builds the scene. For these examples the models are hosted at `https://cdn.rp1.com/sneeze/examples/assets/`.

## The fabric, line by line

Here is the whole file:

```json
{
   "container": "example-stool",
   "services": [],
   "modules":
   [
      {
         "url": "wasm/map.wasm"
      }
   ],
   "data":
   {
      "scene":
      { "Head": { "Self": "P-?" }, "Name": "Stool", "Resource": { "sReference": "assets/Stool.glb" } }
   }
}
```

**`container`** defines a container identifier that is used to group together fabrics that you publish into executable units. Spatial fabrics that you publish with the same container identifier will run in the same container, sharing network connections, storage space, cached files, and console output. If you create separate fabrics that you want to run in separate containers, simply give each one their own identifier. Know that you can only share containers among fabrics that you or your organization publishes. You'll learn more about that a little later.

**`services`** describes the connection settings for outside services that a running module connects to, such as a map or a live data source. This example does not utilize services, so the list is empty. Services are covered in a later example.

**`modules`** lists the programs the fabric runs. This example lists one module, `map.wasm`, which is a general-purpose program that we'll examine in a later example. The job of `map.wasm` is to read a tree of objects out of the `data` section and turn each object into a node in the scene, which is exactly why this fabric can show a stool without you writing any code of your own. If you provide a scene but list no module to interpret it, nothing would be added to the scene.

**`data`** is a general block of information the fabric carries for its modules to read; you can put anything you want in it. The `map.wasm` program we're running looks in one specific place inside it -- **`data.scene`** -- for the tree of objects that makes up the scene. In this particular example, `data.scene` is just a single object. Its three parts are:

- **`Head.Self`** is the object's identifier, written as a class letter, a hyphen, and an index. The letter is the kind of object and the index is which one it is within its container. `P` indicates the node is a physical object, meaning an ordinary solid thing. Here, instead of a fixed number, the index is a `?`, as in `"P-?"`. The `?` tells the engine to assign the next free index in the container automatically, rather than you hard-coding one. This matters because more than one published fabric can be loaded into the same container, and if each hard-coded its own `P-1` the identifiers would collide. Letting the engine hand out the index keeps every object unique no matter how many fabrics share the container. You can still write a fixed index like `"P-1"` when you deliberately want to name a specific object, but `"P-?"` is the safe default.
- **`Name`** is a readable label for the object. Here it is `"Stool"`. It is for your benefit and does not affect what is drawn.
- **`Resource.sReference`** is the address of the 3D model to draw for this object. When the engine builds this node, it downloads this `.glb` file and draws it.

That is the entire scene: a fabric that runs the map module, which reads one physical object, which draws one model.

## Deploying it so the browser can load it

The finished metaverse browser will not (yet) load a fabric from your local disk; it loads it from the network, the same way a web browser loads a page from a web address. Each thing the fabric names - the module in `modules` and the model in `data` - is given as an address, and that address can be written two ways. A full address, one that includes a `scheme://` such as `https://...`, is used exactly as written. Anything shorter is a relative address, resolved against the fabric's own location just as a web browser resolves a relative link on a page: a plain name like `assets/Stool.glb` is looked up in the folder the fabric lives in, a leading `/` starts from the host root, and `..` steps up a folder. That is why this fabric can simply say `wasm/map.wasm` and `assets/Stool.glb` - both sit alongside `stool.json`, so they resolve to `.../sneeze/examples/wasm/map.wasm` and `.../sneeze/examples/assets/Stool.glb`. Either way, every file the fabric references has to end up at an address the browser can reach over the internet.

To deploy this example:

1. Upload `stool.json` to your own web server so it has a public address. A version of this fabric file is also located at `https://cdn.rp1.com/sneeze/examples/stool.json`.
2. Make sure everything the fabric references resolves to a reachable address. Because `wasm/map.wasm` and `assets/Stool.glb` are relative, they must sit beside `stool.json` - here, under `https://cdn.rp1.com/sneeze/examples/`.
3. Give that fabric address to the metaverse browser. The browser will fetch the fabric, then the wasm module and model, and build the scene.

The module and model in this example are already hosted on RP1's CDN, so you can point at those URLs as-is and it will load. For your own fabrics, though, do not depend on RP1's server - host your own copies of the modules and assets on a server you control and change the URLs in the fabric to match. That keeps your fabric working no matter what happens to anyone else's server.

## Where the light comes from

This fabric does not describe any lights -- we'll introduce lighting in the next example. A scene with no light in it would be pure black and invisible, so when a fabric provides no lighting of its own, the engine falls back to a plain ambient light: a soft, even fill that arrives from every direction at once. That fallback is the only reason you can see the stool here at all. Because ambient light has no direction, it reveals the model's colour and form flatly -- there are no bright highlights or cast shadows, since those only appear when light comes from a definite direction. The next examples add lights on purpose, so that you -- not the fallback -- decide how the scene looks and where the shadows fall.

## What is next

Example 02 keeps the same single model but places it deliberately using a position, rotation, and size. That is the groundwork for scenes that hold more than one object, since once you can place one object exactly where you want it, you can place many.
