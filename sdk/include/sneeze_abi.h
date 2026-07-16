// Copyright 2026 Metaversal Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// ===========================================================================
// sneeze_abi.h - the canonical Sneeze WASM ABI contract.
//
// This header is the single source of truth for the wire format shared by the
// Sneeze engine (host) and every guest module, in every language. The host
// includes it to route calls; the Rust SDK mirrors it (see sdk/rust); a future
// C SDK layers ergonomic wrappers on top of it.
//
// (Named sneeze_abi.h, not sneeze.h, so it never collides with the engine's
// public include/Sneeze.h on case-insensitive filesystems.)
//
// The ABI has exactly two crossover functions and a tiny fixed set of guest
// exports, so a module compiled once keeps loading for years as the engine
// evolves (new methods are new numbers, never new symbols):
//
//   Import  (module "Sneeze"):
//     Call    (i32 nOffset, i32 nSize) -> i64               guest -> host request
//
//   Exports (the guest provides):
//     Alloc   (i32 nSize) -> i32 nOffset                    host writes into guest memory here
//     Free    (i32 nOffset, i32 nSize)                      release an Alloc block
//     Notify  (i32 nOffset, i32 nSize) -> i64               host -> guest event (events land later)
//     Init    ()                                            module loaded
//     Open    (i64 twFabricIx, i32 nOffset, i32 nSize)      a fabric opened (snapshot at nOffset)
//     Close   (i64 twFabricIx)                              a fabric closed
//     Shutdown()                                            module unloading
//
// Everything non-trivial is packed into a self-describing packet in the guest's
// own linear memory and routed by a (wType, wMethod) id, rather than resolved by
// one WASM symbol per call.
// ===========================================================================

#ifndef SNEEZE_ABI_H
#define SNEEZE_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version. Bumped only for a breaking change to the packet framing itself
// (never for adding a method - that is just a new wMethod number).
// ---------------------------------------------------------------------------

#define SNEEZE_ABI_VERSION      1

// ---------------------------------------------------------------------------
// Handles. HFABRIC and HNODE are 64-bit value handles (a fabric index and an
// object index respectively - not pointers, since both exceed a wasm32
// pointer). HMAPOBJECT is an opaque pointer to a guest-local builder. C gives
// these no type safety (all uint64_t); the Rust SDK wraps them in newtypes.
// ---------------------------------------------------------------------------

typedef uint64_t   HFABRIC;
typedef uint64_t   HNODE;
typedef uint32_t   HMAPOBJECT;

// ---------------------------------------------------------------------------
// Packet header (8 bytes, little-endian). Every Call/Notify buffer begins with
// this, followed by dwSize bytes of method-specific payload.
// ---------------------------------------------------------------------------

typedef struct tagSNEEZE_ABI_PACKET_HEADER
{
   uint16_t                                                 wType;               // subsystem id (kSNEEZE_ABI_TYPE_*)
   uint16_t                                                 wMethod;             // method id within the subsystem
   uint32_t                                                 dwSize;              // payload byte count following the header
}
SNEEZE_ABI_PACKET_HEADER, *PSNEEZE_ABI_PACKET_HEADER;

// ---------------------------------------------------------------------------
// wType - subsystem registry. Fixed order; numbers are permanent.
// ---------------------------------------------------------------------------

enum eSNEEZE_ABI_TYPE
{
   kSNEEZE_ABI_TYPE_DATA                                 =  1,
   kSNEEZE_ABI_TYPE_CONSOLE                              =  2,
   kSNEEZE_ABI_TYPE_STORAGE                              =  3,
   kSNEEZE_ABI_TYPE_NETWORK                              =  4,
   kSNEEZE_ABI_TYPE_VIEWPORT                             =  5,
   kSNEEZE_ABI_TYPE_SCENE                                =  6,
   kSNEEZE_ABI_TYPE_FABRIC                               =  7,
   kSNEEZE_ABI_TYPE_NODE                                 =  8,
};

// ---------------------------------------------------------------------------
// wMethod - method registry, one enum per subsystem. Numbers are PERMANENT,
// MONOTONIC, and APPEND-ONLY: a revised method takes the next free number,
// never reuses one, and gaps are never backfilled. This is the backward-compat
// guarantee - an old module keeps sending old numbers forever.
// ---------------------------------------------------------------------------

// DATA is the fabric's config "Data" tree, served read-only (no Set/Remove) -
// the immutable analog of STORAGE. Path addressing is identical to STORAGE, but
// there is no scope (the data belongs to the one fabric).
enum eSNEEZE_ABI_METHOD_DATA
{
   kSNEEZE_ABI_METHOD_DATA_HAS                           =  1,
   kSNEEZE_ABI_METHOD_DATA_GET                           =  2,
};

enum eSNEEZE_ABI_METHOD_CONSOLE
{
   kSNEEZE_ABI_METHOD_CONSOLE_LOG                        =  1,
   kSNEEZE_ABI_METHOD_CONSOLE_DEBUG                      =  2,
   kSNEEZE_ABI_METHOD_CONSOLE_INFO                       =  3,
   kSNEEZE_ABI_METHOD_CONSOLE_WARN                       =  4,
   kSNEEZE_ABI_METHOD_CONSOLE_ERROR                      =  5,
   kSNEEZE_ABI_METHOD_CONSOLE_ASSERT                     =  6,
   kSNEEZE_ABI_METHOD_CONSOLE_GROUP                      =  7,
   kSNEEZE_ABI_METHOD_CONSOLE_GROUP_COLLAPSED            =  8,
   kSNEEZE_ABI_METHOD_CONSOLE_GROUP_END                  =  9,
   kSNEEZE_ABI_METHOD_CONSOLE_COUNT                      = 10,
   kSNEEZE_ABI_METHOD_CONSOLE_COUNT_RESET                = 11,
   kSNEEZE_ABI_METHOD_CONSOLE_TIME                       = 12,
   kSNEEZE_ABI_METHOD_CONSOLE_TIME_END                   = 13,
   kSNEEZE_ABI_METHOD_CONSOLE_TIME_LOG                   = 14,
};

enum eSNEEZE_ABI_METHOD_STORAGE
{
   kSNEEZE_ABI_METHOD_STORAGE_HAS                        =  1,
   kSNEEZE_ABI_METHOD_STORAGE_GET                        =  2,
   kSNEEZE_ABI_METHOD_STORAGE_SET                        =  3,
   kSNEEZE_ABI_METHOD_STORAGE_REMOVE                     =  4,
};

enum eSNEEZE_ABI_METHOD_NETWORK
{
   kSNEEZE_ABI_METHOD_NETWORK_FETCH                      =  1,        // not implemented yet (host new)
};

enum eSNEEZE_ABI_METHOD_VIEWPORT
{
   kSNEEZE_ABI_METHOD_VIEWPORT_POSITION_GET              =  1,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_VIEWPORT_POSITION_SET              =  2,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_VIEWPORT_ROTATION_GET              =  3,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_VIEWPORT_ROTATION_SET              =  4,        // not implemented yet (host new)
};

enum eSNEEZE_ABI_METHOD_SCENE
{
   kSNEEZE_ABI_METHOD_SCENE_NODE_ROOT                    =  1,
   kSNEEZE_ABI_METHOD_SCENE_NODE_MAP                     =  2,
   kSNEEZE_ABI_METHOD_SCENE_NODE_OPEN                    =  3,
   kSNEEZE_ABI_METHOD_SCENE_NODE_CLOSE                   =  4,
   kSNEEZE_ABI_METHOD_SCENE_AMBIENT_GET                  =  5,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_SCENE_AMBIENT_SET                  =  6,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_SCENE_DIRECTIONAL_GET              =  7,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_SCENE_DIRECTIONAL_SET              =  8,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_SCENE_BACKGROUND_GET               =  9,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_SCENE_BACKGROUND_SET               = 10,        // not implemented yet (host new)
};

enum eSNEEZE_ABI_METHOD_NODE
{
   kSNEEZE_ABI_METHOD_NODE_POSITION                      =  1,
   kSNEEZE_ABI_METHOD_NODE_ROTATION                      =  2,        // not implemented yet (host new)
   kSNEEZE_ABI_METHOD_NODE_SCALE                         =  3,
   kSNEEZE_ABI_METHOD_NODE_SCALE_AXES                    =  4,
   kSNEEZE_ABI_METHOD_NODE_BOUND                         =  5,
   kSNEEZE_ABI_METHOD_NODE_NAME                          =  6,
   kSNEEZE_ABI_METHOD_NODE_RESOURCE                      =  7,
   kSNEEZE_ABI_METHOD_NODE_PANEL                         =  8,
};

// ---------------------------------------------------------------------------
// Shared enums - mirrors of the engine's own enums (kept in lockstep).
// ---------------------------------------------------------------------------

enum eSNEEZE_ABI_MAP_OBJECT_CLASS
{
   kSNEEZE_ABI_MAP_OBJECT_CLASS_ROOT                     = 70,
   kSNEEZE_ABI_MAP_OBJECT_CLASS_CELESTIAL                = 71,
   kSNEEZE_ABI_MAP_OBJECT_CLASS_TERRESTRIAL              = 72,
   kSNEEZE_ABI_MAP_OBJECT_CLASS_PHYSICAL                 = 73,
   kSNEEZE_ABI_MAP_OBJECT_CLASS_PANEL                    = 74,
   kSNEEZE_ABI_MAP_OBJECT_CLASS_LIGHT                    = 75,
};

enum eSNEEZE_ABI_SILO_SCOPE
{
   kSNEEZE_ABI_SILO_SCOPE_PERMANENT_ORG                  =  0,
   kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER            =  1,
   kSNEEZE_ABI_SILO_SCOPE_TEMPORARY_ORG                  =  2,
   kSNEEZE_ABI_SILO_SCOPE_TEMPORARY_CONTAINER            =  3,
};

enum eSNEEZE_ABI_TRUST
{
   kSNEEZE_ABI_TRUST_NONE                                =  0,
   kSNEEZE_ABI_TRUST_UNTRUSTED                           =  1,
   kSNEEZE_ABI_TRUST_UNVERIFIED                          =  2,
   kSNEEZE_ABI_TRUST_EXPIRED                             =  3,
   kSNEEZE_ABI_TRUST_VERIFIED                            =  4,
   kSNEEZE_ABI_TRUST_ROOT                                =  5,
};

// ---------------------------------------------------------------------------
// OBJECTIX sentinels and composition (mirror of Map_Object.h / Scene.h).
// An OBJECTIX packs a 16-bit class in the high bits and a 48-bit object index
// in the low bits. OBJECTIX_IDENTITY as the index asks the engine to assign the
// next free per-container index ("P-?"). OBJECTIX_ERROR is the failure return.
// ---------------------------------------------------------------------------

#define SNEEZE_OBJECTIX_ERROR                             ((uint64_t) 0x0000FFFFFFFFFFFEull)
#define SNEEZE_OBJECTIX_IDENTITY                          ((uint64_t) 0x0000FFFFFFFFFFFFull)

#define SNEEZE_OBJECTIX_COMPOSE(wClass, twObjectIx)      (((uint64_t) (wClass) << 48)  |  ((uint64_t) (twObjectIx) & 0x0000FFFFFFFFFFFFull))
#define SNEEZE_OBJECTIX_CLASS(qwComposed)                ((uint16_t) ((qwComposed) >> 48))
#define SNEEZE_OBJECTIX_INDEX(qwComposed)                ((uint64_t) (qwComposed) & 0x0000FFFFFFFFFFFFull)

// ---------------------------------------------------------------------------
// SNEEZE_ABI_MAPOBJECT - the 528-byte binary wire struct for a map object. This
// is the one payload that stays raw binary (not field-serialized): builders fill
// it in guest memory, and node-create calls pass its (offset, length). The
// layout mirrors include/Map_Object.h field for field; the static assert guards
// drift. (The engine's internal name for this struct is still RMCOBJECT, being
// migrated off over time.)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
typedef struct tagSNEEZE_ABI_MAPOBJECT
{
   // OBJECT_HEAD (24 bytes)
   uint64_t                                                 qwComposed_Parent;
   uint64_t                                                 qwComposed_Self;
   uint64_t                                                 qwEvent;

   // MAP_OBJECT_NAME (96 bytes) - UTF-16 code units
   uint16_t                                                 wsName[48];

   // MAP_OBJECT_TYPE (8 bytes)
   uint8_t                                                  bType;
   uint8_t                                                  bSubtype;
   uint8_t                                                  bFiction;
   uint8_t                                                  abReserved_Type[5];

   // MAP_OBJECT_OWNER (8 bytes)
   uint64_t                                                 twOwner;

   // MAP_OBJECT_RESOURCE (200 bytes)
   uint64_t                                                 qwResource;
   char                                                     sName_Resource[64];
   char                                                     sReference[128];

   // MAP_OBJECT_TRANSFORM (80 bytes)
   double                                                   d3Position[3];
   double                                                   d4Rotation[4];
   double                                                   d3Scale[3];

   // MAP_OBJECT_ORBIT (32 bytes)
   int64_t                                                  tmPeriod;
   int64_t                                                  tmOrigin;
   double                                                   dA;
   double                                                   dB;

   // MAP_OBJECT_BOUND (48 bytes)
   uint8_t                                                  abReserved_Bound[24];
   double                                                   d3Max[3];

   // MAP_OBJECT_PROPERTIES (32 bytes)
   float                                                    fMass;
   float                                                    fGravity;
   float                                                    fColor;
   float                                                    fBrightness;
   float                                                    fReflectivity;
   uint8_t                                                  abReserved_Properties[12];
}
SNEEZE_ABI_MAPOBJECT, *PSNEEZE_ABI_MAPOBJECT;
#pragma pack(pop)

#define SNEEZE_ABI_MAPOBJECT_SIZE      528

// ---------------------------------------------------------------------------
// Payload wire formats.
//
// Every payload is a sequence of little-endian scalar fields (no struct
// padding is assumed - fields are read/written in order). Conventions:
//   twFabricIx / qwComposed_Parent / qwComposed : u64 (the leading handle)
//   nXxxOffset, nXxxLen                  : i32 into the guest's linear memory
//                                          (a UTF-8 string, or MAPOBJECT bytes)
//   nOutOffset, nOutLen                  : i32 out-buffer for block returns;
//                                          Call returns the full size needed,
//                                          writing min(size, nOutLen) bytes.
//                                          nOutLen == 0 queries the size only.
//   scalars                              : i32 / i64 / f64 as noted
//
// The i64 Call return carries: a created composed identity, an action's 0/1 status,
// a boolean, or (for block getters) the full byte size needed.
//
//   DATA (twFabricIx, then...)  read-only; no scope
//     HAS    : (i32 nPathOffset, i32 nPathLen)                        -> bool
//     GET    : (i32 nPathOffset, i32 nPathLen, i32 nOutOffset, i32 nOutLen) -> size
//
//   CONSOLE (twFabricIx, then...)
//     LOG/DEBUG/INFO/WARN/ERROR/GROUP/GROUP_COLLAPSED/COUNT/COUNT_RESET/
//     TIME/TIME_END/TIME_LOG : (i32 nMsgOffset, i32 nMsgLen)
//     ASSERT                 : (i32 bCondition, i32 nMsgOffset, i32 nMsgLen)
//     GROUP_END              : (no further fields)
//
//   STORAGE (twFabricIx, i32 eScope, then...)
//     HAS    : (i32 nPathOffset, i32 nPathLen)                        -> bool
//     GET    : (i32 nPathOffset, i32 nPathLen, i32 nOutOffset, i32 nOutLen) -> size
//     SET    : (i32 nPathOffset, i32 nPathLen, i32 nValOffset, i32 nValLen) -> 0/1
//     REMOVE : (i32 nPathOffset, i32 nPathLen)                        -> 0/1
//
//   SCENE
//     NODE_ROOT  : (u64 twFabricIx, i32 nObjOffset, i32 nObjLen)      -> qwComposed
//     NODE_MAP   : (u64 twFabricIx, i32 nPathOffset, i32 nPathLen)    -> qwComposed
//     NODE_OPEN  : (i32 nObjOffset, i32 nObjLen)                     -> qwComposed  (parent read from the object's Head.Parent)
//     NODE_CLOSE : (u64 qwComposed)                                   -> 0/1
//
//   NODE (u64 qwComposed, then...)
//     POSITION   : (f64 dX, f64 dY, f64 dZ)
//     SCALE      : (f64 dScale)
//     SCALE_AXES : (f64 dX, f64 dY, f64 dZ)
//     BOUND      : (f64 dX, f64 dY, f64 dZ)
//     NAME       : (i32 nNameOffset, i32 nNameLen)
//     RESOURCE   : (i32 nUrlOffset, i32 nUrlLen)
//     PANEL      : (i32 nRmlOffset, i32 nRmlLen)
//
// Flat C API name reference (the ergonomic C binding, layered on Call by a
// future sneeze.c): Console_Log/Debug/.../TimeLog; Storage_Has/Get/Set/Remove;
// Data_Has/Get; Network_Fetch; Viewport_Position_Get/Set, Viewport_Rotation_Get/Set;
// Scene_Node_Root/Map/Open/Close, Scene_Ambient_Get/Set,
// Scene_Directional_Get/Set, Scene_Background_Get/Set; Node_Position/Rotation/
// Scale/Scale_Axes/Bound/Name/Resource/Panel. Singleton subsystems take
// HFABRIC; node methods take HNODE; builders take HMAPOBJECT.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SNEEZE_ABI_H
