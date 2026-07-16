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

//! ABI constants, mirrored from `sdk/include/sneeze_abi.h` (the single source of
//! truth). Keep the two in lockstep: a `wMethod` number is permanent, monotonic,
//! and append-only.

pub const SNEEZE_ABI_VERSION                                : u32 = 1;

// wType - subsystem registry.
pub const kSNEEZE_ABI_TYPE_DATA                             : u16 =  1;
pub const kSNEEZE_ABI_TYPE_CONSOLE                          : u16 =  2;
pub const kSNEEZE_ABI_TYPE_STORAGE                          : u16 =  3;
pub const kSNEEZE_ABI_TYPE_NETWORK                          : u16 =  4;
pub const kSNEEZE_ABI_TYPE_VIEWPORT                         : u16 =  5;
pub const kSNEEZE_ABI_TYPE_SCENE                            : u16 =  6;
pub const kSNEEZE_ABI_TYPE_FABRIC                           : u16 =  7;
pub const kSNEEZE_ABI_TYPE_NODE                             : u16 =  8;

// CONSOLE methods.
pub const kSNEEZE_ABI_METHOD_CONSOLE_LOG                    : u16 = 1;
pub const kSNEEZE_ABI_METHOD_CONSOLE_DEBUG                  : u16 = 2;
pub const kSNEEZE_ABI_METHOD_CONSOLE_INFO                   : u16 = 3;
pub const kSNEEZE_ABI_METHOD_CONSOLE_WARN                   : u16 = 4;
pub const kSNEEZE_ABI_METHOD_CONSOLE_ERROR                  : u16 = 5;
pub const kSNEEZE_ABI_METHOD_CONSOLE_ASSERT                 : u16 = 6;
pub const kSNEEZE_ABI_METHOD_CONSOLE_GROUP                  : u16 = 7;
pub const kSNEEZE_ABI_METHOD_CONSOLE_GROUP_COLLAPSED        : u16 = 8;
pub const kSNEEZE_ABI_METHOD_CONSOLE_GROUP_END              : u16 = 9;
pub const kSNEEZE_ABI_METHOD_CONSOLE_COUNT                  : u16 = 10;
pub const kSNEEZE_ABI_METHOD_CONSOLE_COUNT_RESET            : u16 = 11;
pub const kSNEEZE_ABI_METHOD_CONSOLE_TIME                   : u16 = 12;
pub const kSNEEZE_ABI_METHOD_CONSOLE_TIME_END               : u16 = 13;
pub const kSNEEZE_ABI_METHOD_CONSOLE_TIME_LOG               : u16 = 14;

// STORAGE methods.
pub const kSNEEZE_ABI_METHOD_STORAGE_HAS                    : u16 = 1;
pub const kSNEEZE_ABI_METHOD_STORAGE_GET                    : u16 = 2;
pub const kSNEEZE_ABI_METHOD_STORAGE_SET                    : u16 = 3;
pub const kSNEEZE_ABI_METHOD_STORAGE_REMOVE                 : u16 = 4;

// NETWORK methods (not implemented yet host-side).
pub const kSNEEZE_ABI_METHOD_NETWORK_FETCH                  : u16 = 1;

// VIEWPORT methods (not implemented yet host-side).
pub const kSNEEZE_ABI_METHOD_VIEWPORT_POSITION_GET          : u16 = 1;
pub const kSNEEZE_ABI_METHOD_VIEWPORT_POSITION_SET          : u16 = 2;
pub const kSNEEZE_ABI_METHOD_VIEWPORT_ROTATION_GET          : u16 = 3;
pub const kSNEEZE_ABI_METHOD_VIEWPORT_ROTATION_SET          : u16 = 4;

// SCENE methods (globals 5..10 not implemented yet host-side).
pub const kSNEEZE_ABI_METHOD_SCENE_NODE_ROOT                : u16 = 1;
pub const kSNEEZE_ABI_METHOD_SCENE_NODE_MAP                 : u16 = 2;
pub const kSNEEZE_ABI_METHOD_SCENE_NODE_OPEN                : u16 = 3;
pub const kSNEEZE_ABI_METHOD_SCENE_NODE_CLOSE               : u16 = 4;
pub const kSNEEZE_ABI_METHOD_SCENE_AMBIENT_GET              : u16 = 5;
pub const kSNEEZE_ABI_METHOD_SCENE_AMBIENT_SET              : u16 = 6;
pub const kSNEEZE_ABI_METHOD_SCENE_DIRECTIONAL_GET          : u16 = 7;
pub const kSNEEZE_ABI_METHOD_SCENE_DIRECTIONAL_SET          : u16 = 8;
pub const kSNEEZE_ABI_METHOD_SCENE_BACKGROUND_GET           : u16 = 9;
pub const kSNEEZE_ABI_METHOD_SCENE_BACKGROUND_SET           : u16 = 10;

// NODE methods (ROTATION 2 reserved, not yet implemented host-side).
pub const kSNEEZE_ABI_METHOD_NODE_POSITION                  : u16 = 1;
pub const kSNEEZE_ABI_METHOD_NODE_ROTATION                  : u16 = 2;
pub const kSNEEZE_ABI_METHOD_NODE_SCALE                     : u16 = 3;
pub const kSNEEZE_ABI_METHOD_NODE_SCALE_AXES                : u16 = 4;
pub const kSNEEZE_ABI_METHOD_NODE_BOUND                     : u16 = 5;
pub const kSNEEZE_ABI_METHOD_NODE_NAME                      : u16 = 6;
pub const kSNEEZE_ABI_METHOD_NODE_RESOURCE                  : u16 = 7;
pub const kSNEEZE_ABI_METHOD_NODE_PANEL                     : u16 = 8;

// DATA methods (read-only; the immutable analog of STORAGE).
pub const kSNEEZE_ABI_METHOD_DATA_HAS                       : u16 = 1;
pub const kSNEEZE_ABI_METHOD_DATA_GET                       : u16 = 2;

// TRUST levels - the container's verification standing (Container.eTrust in the
// Open snapshot). Ordered least-to-most trusted.
pub const kSNEEZE_ABI_TRUST_NONE                            : i32 = 0;
pub const kSNEEZE_ABI_TRUST_UNTRUSTED                       : i32 = 1;
pub const kSNEEZE_ABI_TRUST_UNVERIFIED                      : i32 = 2;
pub const kSNEEZE_ABI_TRUST_EXPIRED                         : i32 = 3;
pub const kSNEEZE_ABI_TRUST_VERIFIED                        : i32 = 4;
pub const kSNEEZE_ABI_TRUST_ROOT                            : i32 = 5;

// SILO scope - selects one of a SILO's four storage units.
#[repr(i32)]
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum eSNEEZE_ABI_SILO_SCOPE
{
   kSNEEZE_ABI_SILO_SCOPE_PERMANENT_ORG                           = 0,
   kSNEEZE_ABI_SILO_SCOPE_PERMANENT_CONTAINER                     = 1,
   kSNEEZE_ABI_SILO_SCOPE_TEMPORARY_ORG                           = 2,
   kSNEEZE_ABI_SILO_SCOPE_TEMPORARY_CONTAINER                     = 3,
}

// MAP_OBJECT class ids.
pub const kSNEEZE_ABI_MAP_OBJECT_CLASS_ROOT                 : u16 = 70;
pub const kSNEEZE_ABI_MAP_OBJECT_CLASS_CELESTIAL            : u16 = 71;
pub const kSNEEZE_ABI_MAP_OBJECT_CLASS_TERRESTRIAL          : u16 = 72;
pub const kSNEEZE_ABI_MAP_OBJECT_CLASS_PHYSICAL             : u16 = 73;
pub const kSNEEZE_ABI_MAP_OBJECT_CLASS_PANEL                : u16 = 74;
pub const kSNEEZE_ABI_MAP_OBJECT_CLASS_LIGHT                : u16 = 75;

// OBJECTIX sentinels + composition (macros in the C header, hence SNEEZE_* not
// kSNEEZE_ABI_*).
pub const SNEEZE_OBJECTIX_ERROR                             : u64 = 0x0000_FFFF_FFFF_FFFE;
pub const SNEEZE_OBJECTIX_IDENTITY                          : u64 = 0x0000_FFFF_FFFF_FFFF;

pub const fn SNEEZE_OBJECTIX_COMPOSE (wClass: u16, twObjectIx: u64) -> u64 { ((wClass as u64) << 48)  |  (twObjectIx & 0x0000_FFFF_FFFF_FFFF) }
pub const fn SNEEZE_OBJECTIX_CLASS   (             qwComposed: u64) -> u16 {                             (qwComposed >> 48) as u16 }
pub const fn SNEEZE_OBJECTIX_INDEX   (             qwComposed: u64) -> u64 {                             (qwComposed & 0x0000_FFFF_FFFF_FFFF) }
