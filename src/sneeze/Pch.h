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

#ifndef SNEEZE_PCH_H
#define SNEEZE_PCH_H

// ---------------------------------------------------------------------------
// Sneeze precompiled header.
// CMake's target_precompile_headers force-includes this at the top of every
// .cpp in the Sneeze static library. Only add headers here that are (a) used
// by many translation units, and (b) heavy enough to justify the cost --
// every source file rebuilds when this header changes.
// ---------------------------------------------------------------------------

// --- Standard library ---

#include <unordered_map>
#include <map>
#include <queue>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <sstream>
#include <random>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/pem.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#undef X509_NAME
#undef X509_EXTENSIONS

#pragma comment (lib, "winmm.lib")
#endif

// --- Sneeze public umbrella (included by almost every .cpp) ---

#include <Sneeze.h>

#include <RMAP/RMAP.h>
#include <RMAP_Svc_SB/RMAP_Svc_SB.h>
#include <RMAP_Svc_Rest/RMAP_Svc_Rest.h>
#include <RMAP_Svc_SocketIO/RMAP_Svc_SocketIO.h>
#include <Map/Map.h>

std::string NowIso8601 ();

#endif // SNEEZE_PCH_H
