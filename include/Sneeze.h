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

#ifndef SNEEZE_SNEEZE_H
#define SNEEZE_SNEEZE_H

#include <nlohmann/json.hpp>
#include <vector>
#include <string>

namespace SNEEZE
{
   class ENGINE;
   class CONTEXT;
   class JOB_FETCH;
   class JOB_COMPOSITOR;
   class ICONTEXT;
   class IVIEWPORT;

   namespace DEP
   {
      class WASM_RUNTIME;
      class UI_CONTEXT;
      class XR_RUNTIME;
   }
}

#include <Map/Map_Object.h>
#include "Container.h"
#include "Context.h"
#include "Msf.h"
#include "Console.h"
#include "Network.h"
#include "Storage.h"
#include "Scene.h"
#include "Viewport.h"
#include "Persona.h"
#include "XrTracking.h"

namespace SNEEZE
{
   // ------------------------------------------------------------------------
   // IENGINE -- interface between the host application and the engine.
   // Engine-level only: logging, data paths, renderer selection.
   // ------------------------------------------------------------------------

   class IENGINE
   {
   public:
      enum eLOGLEVEL
      {
         kLOGLEVEL_Trace,
         kLOGLEVEL_Info,
         kLOGLEVEL_Warning,
         kLOGLEVEL_Error
      };

   public:
      virtual ~IENGINE () = default;

      // Accessors
      virtual std::string const& sAppDataPath ()       const& = 0;
      virtual std::string const& sRenderer ()          const& = 0;

      // Methods
      virtual void Log (eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage) = 0;
   };

   // ------------------------------------------------------------------------
   // ICONTEXT -- per-context session interface.
   // Passed to ENGINE::Context_Open(). Inspector callbacks only.
   // ------------------------------------------------------------------------

   class ICONTEXT
   {
   public:
      virtual ~ICONTEXT () = default;

      virtual void OnContainerCreated     (CONTAINER*) {}
      virtual void OnContainerDeleted     (CONTAINER*) {}

      virtual void OnNetworkCacheCreated  (CACHE*) {}
      virtual void OnNetworkCacheDeleted  (CACHE*) {}

      virtual bool OnNetworkFileCreated   (FILE*) { return true; }
      virtual void OnNetworkFileChanged   (FILE*) {}
      virtual void OnNetworkFileDeleted   (FILE*) {}

      virtual void OnStorageSiloCreated   (SILO*) {}
      virtual void OnStorageSiloDeleted   (SILO*) {}

      virtual void OnStorageUnitCreated   (SILO*, eSILO_SCOPE eScope) {}
      virtual void OnStorageUnitChanged   (SILO*, eSILO_SCOPE eScope, const std::string&) {}
      virtual void OnStorageUnitDeleted   (SILO*, eSILO_SCOPE eScope) {}

      virtual void OnConsoleEntryCreated  (std::shared_ptr<const ENTRY>) {}
      virtual void OnConsoleEntryDeleted  (std::shared_ptr<const ENTRY>) {}
   };

   // ------------------------------------------------------------------------
   // IVIEWPORT -- per-viewport rendering interface.
   // Passed to VIEWPORT::Activate(). Rendering callbacks only.
   // ------------------------------------------------------------------------

   class IVIEWPORT
   {
   public:
      virtual ~IVIEWPORT () = default;

      virtual void* FrameWindow () = 0;
      virtual bool  FrameSize (int& nWidth, int& nHeight) = 0;

      virtual void  OnFrameReady (const uint32_t* pFB, int nFbW, int nFbH) = 0;
   };

   // ------------------------------------------------------------------------
   // ENGINE
   // ------------------------------------------------------------------------

   class ENGINE
   {
   public:
      struct CONFIG
      {
         bool                          bBoundingBox;   // True = Show Bounding Boxes for Nodes, False = Hides them
      };

   public:
      static constexpr const char* sFOLDER_PERSISTENT = "Persistent";
      static constexpr const char* sFOLDER_TRANSITORY = "Transitory";

      // Constructors, assignments, and destructor
      explicit ENGINE (IENGINE* pHost);

      ENGINE & operator=(ENGINE const  & rhs)   = delete;
      ENGINE & operator=(ENGINE       && rhs)   = delete;
      ENGINE            (ENGINE const  & other) = delete;
      ENGINE            (ENGINE       && other) = delete;
      ~ENGINE           ();

      IENGINE* Host () const;

      bool Initialize (const CONFIG& Config = CONFIG {});

      void SetConfig (const CONFIG& Config);
      void GetConfig (CONFIG& Config);

         // --- Context management ---

      CONTEXT*                       Context_Open    (ICONTEXT* pHost, const std::string& sUrl = "", CONTEXT::eSESSION kSession = CONTEXT::kSESSION_PERSISTENT, bool bReset = false);
      bool                           Context_Close   (CONTEXT* pContext);

      // --- Shared services ---

      void                     Log (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage);

      // --- Persona ---

      void Login (const std::string& sFirst, const std::string& sSecond);
      void Logout ();
      void ChangePersona (const std::string& sFirst, const std::string& sSecond);

      // --- Paths ---

      const std::string&       Path_Persistent () const;
      const std::string&       Path_Session () const;

      // --- Subsystems ---

      persona::PERSONA*        Persona () const;
      DEP::WASM_RUNTIME*       Wasm_Runtime () const;
      DEP::UI_CONTEXT*         Ui_Context () const;
      DEP::XR_RUNTIME*         Xr () const;
      NETWORK*                 Network () const;
      STORAGE*                 Storage () const;
      CONSOLE*                 Console () const;

      // OpenXR face/body (Galaxy XR / fixtures). Safe when no runtime — fixtures still work.
      XR_CAPABILITIES          XrCapabilities () const;
      bool                     XrPollFace (XR_FACE_STATE& outFace) const;
      bool                     XrPollBody (XR_BODY_STATE& outBody) const;
      void                     XrInjectFaceFixture (const XR_FACE_STATE& face);
      void                     XrInjectBodyFixture (const XR_BODY_STATE& body);
      void                     XrClearFixtures ();
      void                     XrSetAvatarBindId (const std::string& sId);
      std::string              XrAvatarBindId () const;
      void                     Queue_Post_Fetch      (JOB_FETCH* pJob_Fetch);
      void                     Queue_Post_Compositor (JOB_COMPOSITOR* pJob_Compositor);

   private:
      class Impl;
      Impl* m_pImpl;
   };

   /// Drop in-process glTF URL cache (fabric reload / hard refresh).
   void Gltf_Render_Model_ClearCache ();
}

#endif // SNEEZE_SNEEZE_H
