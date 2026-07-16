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

#include "wasm/Wasm.h"
#include "scene/RmcObject.h"

using namespace SNEEZE;

// Structural key of the SOM node schema: a node's child array. Unlike the flat
// RMCOBJECT field keys (which live in scene/RmcObject.cpp), "aChildren" is the
// tree's concern -- the branch walker reads it to recurse; it is not part of the
// wire object.
#define NODE_KEY_CHILDREN "aChildren"

// ============================================================================
// CONTAINER::CID
// ============================================================================

std::string CONTAINER::CID::DisplayName () const
{
   return ((eTrust >= kTRUST_EXPIRED) ? sOrganization : sOrganizationHash) + "/" + sContainer;
}

std::string CONTAINER::CID::Key_Org () const
{
   std::string sPersona = (sPersonaHash.size () >= 12) ? sPersonaHash.substr (0, 12) : sPersonaHash;
   std::string sFp2     = (sFingerprint.size () >=  2) ? sFingerprint.substr (0,  2) : sFingerprint;
   std::string sFp22    = (sFingerprint.size () >= 24) ? sFingerprint.substr (2, 22) : std::string ();

   return sPersona + "/" + sFp2 + "/" + sFp22;
}

std::string CONTAINER::CID::Key_All () const
{
   return Key_Org () + "/" + sContainer;
}


// ============================================================================
// CONTAINER::Impl
// ============================================================================

class CONTAINER::Impl
{
public:

   Impl (CONTAINER* pContainer, CONTEXT* pContext, const CID* pCID) :
      m_pContainer          (pContainer),
      m_pContext            (pContext),
      m_CID                 (*pCID),
      m_sKey_Org            (m_CID.Key_Org ()),
      m_sKey_All            (m_CID.Key_All ()),
      m_sPath_Permanent_Org ((std::filesystem::path (pContext->Path_Permanent ()) / m_sKey_Org).generic_string ()),
      m_sPath_Temporary_Org ((std::filesystem::path (pContext->Path_Temporary ()) / m_sKey_Org).generic_string ()),
      m_sPath_Permanent_All ((std::filesystem::path (pContext->Path_Permanent ()) / m_sKey_All).generic_string ()),
      m_sPath_Temporary_All ((std::filesystem::path (pContext->Path_Temporary ()) / m_sKey_All).generic_string ()),
      m_nCount_Open         (0),
      m_pCache              (nullptr),
      m_pSilo               (nullptr),
      m_pStream             (nullptr),
      m_pWasm_Store         (nullptr),
      m_twObjectIx_Next     (0)
   {
   }

  ~Impl ()
   {
      if (m_nCount_Open > 0)
         m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "CONTAINER", "Destroyed with refcount " + std::to_string (m_nCount_Open) + " — " + m_CID.DisplayName ());

      for (auto* pMapObj : m_apMap_Object)
         delete pMapObj;
      m_apMap_Object.clear();
  }

   // -----------------------------------------------------------------------
   // Lifecycle
   // -----------------------------------------------------------------------

   bool Open (bool bReset)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxContainer);

      bool bResult = false;

      if (m_CID.eTrust == kTRUST_ROOT  &&  bReset)
         m_sTime_Stale = NowIso8601 ();

      if (m_nCount_Open++ == 0)
      {
         std::error_code ec;
         std::filesystem::create_directories (m_sPath_Permanent_All, ec);
         std::filesystem::create_directories (m_sPath_Temporary_All, ec);

         if ((m_pCache = m_pContext->Network ()->Cache_Open (m_pContainer)))
         {
            if ((m_pStream = m_pContext->Console ()->Stream_Open (m_pContainer)))
            {
               if ((m_pSilo = m_pContext->Storage ()->Silo_Open (m_pContainer)))
               {
                  m_pSilo->Attach ();

                  if ((m_pWasm_Store = m_pContext->Wasm_Runtime ()->Store_Open ()))
                  {
                     m_pWasm_Store->HostData (static_cast<void*> (m_pContainer));
                     m_pWasm_Store->Linker_Initialize ();

                     m_pContext->Host ()->OnContainerCreated (m_pContainer);

                     bResult = true;
                  }
               }
            }
         }
      }
      else bResult = true;

      if (!bResult)
         Close ();

      return bResult;
   }

   size_t Close ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxContainer);

      if (--m_nCount_Open == 0)
      {
         m_pContext->Host ()->OnContainerDeleted (m_pContainer);

         if (m_pWasm_Store)
         {
            m_pContext->Wasm_Runtime ()->Store_Close (m_pWasm_Store);
            m_pWasm_Store = nullptr;
         }

         if (m_pSilo)
         {
            m_pSilo->Detach ();

            m_pContext->Storage ()->Silo_Close (m_pContainer, m_pSilo);
            m_pSilo = nullptr;
         }

         if (m_pStream)
         {
            m_pContext->Console ()->Stream_Close (m_pStream);
            m_pStream = nullptr;
         }

         if (m_pCache)
         {
            m_pContext->Network ()->Cache_Close (m_pContainer, m_pCache);
            m_pCache = nullptr;
         }
      }

      return m_nCount_Open;
   }

   std::string Reset_Stale () const
   {
      std::string sResult;

      if (m_CID.eTrust == kTRUST_ROOT)
      {
         if (!m_sTime_Stale.empty ())
            sResult = m_sTime_Stale;
         else sResult = m_pContext->Network ()->Time_Start ();
      }

      return sResult;
   }

   // -----------------------------------------------------------------------
   // WASM Instance Lifecycle
   // -----------------------------------------------------------------------

   bool Instance_Open (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash, const std::vector<uint8_t>& aWasmBytes, const std::vector<uint8_t>& aSnapshot)
   {
      return m_pWasm_Store->Instance_Open (twFabricIx, sUrl, sHash, aWasmBytes.data (), aWasmBytes.size (), aSnapshot.data (), aSnapshot.size ());
   }

   void Instance_Close (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash)
   {
      m_pWasm_Store->Instance_Close (twFabricIx, sUrl, sHash);
   }

   // -----------------------------------------------------------------------
   // Internal Node management
   // -----------------------------------------------------------------------

   // -----------------------------------------------------------------------
   // Scene Node Handle Table
   //
   // REVISIT: Fabrics will operate in one of two mutually exclusive modes:
   // (a) WASM-managed — the WASM code builds the scene graph via Node_Root
   //     and Node_Open, or
   // (b) Map-managed — the WASM code delegates to a map service, and the
   //     browser manages the root node on the fabric's behalf.
   //
   // When the same MSF is loaded into multiple fabrics under the same
   // container, WASM-managed mode requires unique node indices per fabric
   // (the current per-container map cannot hold duplicate template indices).
   // See Scene.md "Fabric Ownership Modes" for the full discussion.
   // -----------------------------------------------------------------------

   uint64_t Node_Root (uint64_t twFabricIx, const RMCOBJECT* pRMCObject)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxContainer);

      uint64_t twObjectIx = OBJECTIX_ERROR;

      if (pRMCObject)
      {
         FABRIC* pFabric = m_pContext->Scene ()->Fabric_Find (twFabricIx);

         if (pFabric  &&  pFabric->Node_Root () == nullptr)
            twObjectIx = Node_Create (pFabric, nullptr, pRMCObject);
      }

      return twObjectIx;
   }

   uint64_t Node_Open (const RMCOBJECT* pRMCObject)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxContainer);

      uint64_t twObjectIx = OBJECTIX_ERROR;

      if (pRMCObject)
      {
         NODE* pNode_Parent = Node_Find (pRMCObject->Head.Parent.qwComposed);

         if (pNode_Parent)
            twObjectIx = Node_Create (pNode_Parent->Fabric (), pNode_Parent, pRMCObject);
      }

      return twObjectIx;
   }

   uint64_t Node_Create (FABRIC* pFabric, NODE* pNode_Parent, const RMCOBJECT* pRMCObject)
   {
      MAP_OBJECT::OBJECT_HEAD      Head       = pRMCObject->Head;
      MAP_OBJECT::MAP_OBJECT_CLASS eClass     = pRMCObject->Head.Self.Class ();
      uint64_t         twObjectIx = pRMCObject->Head.Self.ObjectIx ();

      if (twObjectIx == OBJECTIX_IDENTITY)
      {
         if (m_twObjectIx_Next < OBJECTIX_MAX)
         {
            twObjectIx           = ++m_twObjectIx_Next;

            Head.Self.qwComposed = OBJECTIX_COMPOSE (eClass, twObjectIx);
         }
      }
      else if (twObjectIx > OBJECTIX_NULL  &&  twObjectIx <= OBJECTIX_MAX)
      {
         if (m_umpNode.find (Head.Self.qwComposed) == m_umpNode.end ())
         {
            if (m_twObjectIx_Next < twObjectIx)
               m_twObjectIx_Next = twObjectIx;
         }
         else twObjectIx = OBJECTIX_NULL;
      }

      if (twObjectIx > OBJECTIX_NULL  &&  twObjectIx <= OBJECTIX_MAX)
      {
         MAP_OBJECT* pMapObj = nullptr;

         switch (eClass)
         {
            case MAP_OBJECT::MAP_OBJECT_CLASS_ROOT:        pMapObj = new MAP_OBJECT_ROOT        (Head);  break;
            case MAP_OBJECT::MAP_OBJECT_CLASS_CELESTIAL:   pMapObj = new MAP_OBJECT_CELESTIAL   (Head);  break;
            case MAP_OBJECT::MAP_OBJECT_CLASS_TERRESTRIAL: pMapObj = new MAP_OBJECT_TERRESTRIAL (Head);  break;
            case MAP_OBJECT::MAP_OBJECT_CLASS_PHYSICAL:    pMapObj = new MAP_OBJECT_PHYSICAL    (Head);  break;
            case MAP_OBJECT::MAP_OBJECT_CLASS_PANEL:       pMapObj = new MAP_OBJECT_PANEL       (Head);  break;
            case MAP_OBJECT::MAP_OBJECT_CLASS_LIGHT:       pMapObj = new MAP_OBJECT_LIGHT       (Head);  break;
         }

         if (pMapObj)
         {
            pMapObj->Name       = pRMCObject->Name;
            pMapObj->Type       = pRMCObject->Type;
            pMapObj->Resource   = pRMCObject->Resource;
            pMapObj->Transform  = pRMCObject->Transform;
            pMapObj->Orbit      = pRMCObject->Orbit;
            pMapObj->Bound      = pRMCObject->Bound;
            pMapObj->Properties = pRMCObject->Properties;

            auto* pNode = new NODE (pFabric, pNode_Parent, Head.Self.qwComposed);

            pNode->Initialize (pMapObj);

            m_umpNode[Head.Self.qwComposed] = pNode;
            m_apMap_Object.push_back (pMapObj);
         }
         else Head.Self.qwComposed = OBJECTIX_ERROR;
      }
      else Head.Self.qwComposed = OBJECTIX_ERROR;

      return Head.Self.qwComposed;
   }

   bool Node_Close (uint64_t twObjectIx)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxContainer);

      bool  bResult = false;
      NODE* pNode   = Node_Find (twObjectIx);

      if (pNode)
      {
         MAP_OBJECT* pMapObj = pNode->Map_Object ();

         m_umpNode.erase (twObjectIx);

         delete pNode;

         if (pMapObj)
         {
            auto it = std::find (m_apMap_Object.begin (), m_apMap_Object.end (), pMapObj);
            if (it != m_apMap_Object.end ())
               m_apMap_Object.erase (it);

            delete pMapObj;
         }

         bResult = true;
      }

      return bResult;
   }

   NODE* Node_Find (uint64_t twObjectIx) const
   {
      NODE* pNode = nullptr;

      auto it = m_umpNode.find (twObjectIx);
      if (it != m_umpNode.end ())
         pNode = it->second;

      return pNode;
   }

   // -----------------------------------------------------------------------
   // Scene-branch injection
   //
   // Build a scene subtree from a JSON node branch (an MSF "Data" tree, or any
   // JSON of the same shape). The entrypoint attaches the branch as a new
   // fabric root; the NODE* overload is the recursion, attaching a node's
   // "aChildren" under an already-created parent and returning the count of
   // nodes it added. Both run under m_mxContainer (the entrypoint takes it;
   // the recursion is only ever reached from there).
   // -----------------------------------------------------------------------

   uint64_t Branch_Add (uint64_t twFabricIx, const nlohmann::json& jBranch)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxContainer);

      uint64_t twResult = OBJECTIX_ERROR;

      if (jBranch.is_object ())
      {
         RMCOBJECT RMCObject;
         RmcObject_FromJson (jBranch, &RMCObject);

         uint64_t twRootIx = Node_Root (twFabricIx, &RMCObject);

         if (twRootIx != OBJECTIX_ERROR)
         {
            uint32_t nCount = 1 + Branch_Add_Aux (Node_Find (twRootIx), jBranch);

            m_pContext->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "MAP", "Injected " + std::to_string (nCount) + " nodes");

            twResult = twRootIx;
         }
      }

      return twResult;
   }

   uint32_t Branch_Add_Aux (NODE* pParent, const nlohmann::json& jParent)
   {
      uint32_t nCount = 0;

      if (pParent  &&  jParent.contains (NODE_KEY_CHILDREN)  &&  jParent[NODE_KEY_CHILDREN].is_array ())
      {
         for (const auto& jChild : jParent[NODE_KEY_CHILDREN])
         {
            RMCOBJECT RMCObject;
            RmcObject_FromJson (jChild, &RMCObject);

            uint64_t twChildIx = Node_Create (pParent->Fabric (), pParent, &RMCObject);

            if (twChildIx != OBJECTIX_ERROR)
               nCount += 1 + Branch_Add_Aux (Node_Find (twChildIx), jChild);
         }
      }

      return nCount;
   }

   // -----------------------------------------------------------------------
   // Members
   // -----------------------------------------------------------------------

   CONTAINER*                            m_pContainer;
   CONTEXT*                              m_pContext;
   CID                                   m_CID;
   std::string                           m_sKey_Org;
   std::string                           m_sKey_All;
   std::string                           m_sPath_Permanent_Org;
   std::string                           m_sPath_Temporary_Org;
   std::string                           m_sPath_Permanent_All;
   std::string                           m_sPath_Temporary_All;

   uint32_t                              m_nCount_Open;
   std::recursive_mutex                  m_mxContainer;
   std::string                           m_sTime_Stale;

   STREAM*                               m_pStream;
   SILO*                                 m_pSilo;
   CACHE*                                m_pCache;
   DEP::WASM_STORE*                      m_pWasm_Store;

   uint64_t                              m_twObjectIx_Next;
   std::unordered_map<uint64_t, NODE*>   m_umpNode;
   std::vector<MAP_OBJECT*>              m_apMap_Object;
};


// ============================================================================
// CONTAINER
// ============================================================================

CONTAINER::CONTAINER (CONTEXT* pContext, const CID* pCID) :
   m_pImpl (new Impl (this, pContext, pCID))
{
}

CONTAINER::~CONTAINER ()
{
   delete m_pImpl;
}

bool                  CONTAINER::Open        (bool bReset)                       { return  m_pImpl->Open  (bReset); }
size_t                CONTAINER::Close       ()                                  { return  m_pImpl->Close (); }
std::string           CONTAINER::Reset_Stale () const                            { return  m_pImpl->Reset_Stale (); }

SNEEZE::CONTEXT*      CONTAINER::Context     () const                            { return  m_pImpl->m_pContext; }
const CONTAINER::CID* CONTAINER::Identity    () const                            { return &m_pImpl->m_CID; }
const std::string&    CONTAINER::Key         () const                            { return  m_pImpl->m_sKey_All; }
STREAM*               CONTAINER::Stream      () const                            { return  m_pImpl->m_pStream; }
SILO*                 CONTAINER::Silo        () const                            { return  m_pImpl->m_pSilo; }
CACHE*                CONTAINER::Cache       () const                            { return  m_pImpl->m_pCache; }

const std::string&    CONTAINER::Path_Permanent_Org () const                     { return  m_pImpl->m_sPath_Permanent_Org; }
const std::string&    CONTAINER::Path_Temporary_Org () const                     { return  m_pImpl->m_sPath_Temporary_Org; }
const std::string&    CONTAINER::Path_Permanent_All () const                     { return  m_pImpl->m_sPath_Permanent_All; }
const std::string&    CONTAINER::Path_Temporary_All () const                     { return  m_pImpl->m_sPath_Temporary_All; }

bool CONTAINER::Instance_Open (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash, const std::vector<uint8_t>& aWasmBytes, const std::vector<uint8_t>& aSnapshot)
{
   return m_pImpl->Instance_Open  (twFabricIx, sUrl, sHash, aWasmBytes, aSnapshot);
}

void CONTAINER::Instance_Close (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash)
{
   m_pImpl->Instance_Close (twFabricIx, sUrl, sHash);
}

uint64_t CONTAINER::Node_Root  (uint64_t twFabricIx, const RMCOBJECT* pRMCObject)   { return m_pImpl->Node_Root  (twFabricIx, pRMCObject); }
uint64_t CONTAINER::Node_Open  (                     const RMCOBJECT* pRMCObject)   { return m_pImpl->Node_Open  (pRMCObject); }
bool     CONTAINER::Node_Close (uint64_t twObjectIx)                                { return m_pImpl->Node_Close (twObjectIx); }
NODE*    CONTAINER::Node_Find  (uint64_t twObjectIx) const                          { return m_pImpl->Node_Find  (twObjectIx); }
uint64_t CONTAINER::Branch_Add (uint64_t twFabricIx, const nlohmann::json& jBranch) { return m_pImpl->Branch_Add (twFabricIx, jBranch); }
