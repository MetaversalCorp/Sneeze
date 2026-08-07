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

#include "wasm/Chrono.h"

using namespace SNEEZE;

// ---------------------------------------------------------------------------
// URL resolution — resolve a reference against the fabric's own URL using the
// standard relative-reference rules (RFC 3986). A reference that carries a
// "scheme://" is absolute and used unchanged; a reference beginning with "/"
// is taken from the host root; anything else is relative to the folder holding
// the fabric. "." and ".." segments are collapsed the usual way.
// ---------------------------------------------------------------------------

static std::string RemoveDotSegments (const std::string& sPath)
{
   std::vector<std::string> apSeg;
   size_t                   nStart = 0;
   size_t                   nLen   = sPath.length ();
   bool                     bTrail = false;

   while (nStart < nLen)
   {
      size_t      nSlash = sPath.find ('/', nStart);
      std::string sSeg   = (nSlash == std::string::npos) ? sPath.substr (nStart) : sPath.substr (nStart, nSlash - nStart);

      if (sSeg == "..")
      {
         if (!apSeg.empty ())
            apSeg.pop_back ();

         bTrail = true;
      }
      else if (sSeg == ".")
      {
         bTrail = true;
      }
      else if (!sSeg.empty ())
      {
         apSeg.push_back (sSeg);

         bTrail = false;
      }

      nStart = (nSlash == std::string::npos) ? nLen : nSlash + 1;
   }

   std::string sResult = "/";

   for (size_t i = 0; i < apSeg.size (); ++i)
   {
      sResult += apSeg[i];

      if ((i + 1 < apSeg.size ())  ||  bTrail)
         sResult += "/";
   }

   return sResult;
}

static std::string ResolveUrl (const std::string& sBase, const std::string& sReference)
{
   std::string sResult      = sReference;
   size_t      nRefScheme   = sReference.find ("://");
   size_t      nBaseScheme  = sBase.find ("://");

   if (nRefScheme == std::string::npos  &&  nBaseScheme != std::string::npos)
   {
      size_t      nAuthority = nBaseScheme + 3;
      size_t      nPath      = sBase.find ('/', nAuthority);
      std::string sOrigin    = (nPath == std::string::npos) ? sBase : sBase.substr (0, nPath);
      std::string sBasePath  = (nPath == std::string::npos) ? "/"   : sBase.substr (nPath);
      std::string sRefPath;

      if (!sReference.empty ()  &&  sReference[0] == '/')
      {
         sRefPath = sReference;
      }
      else
      {
         size_t      nLastSlash = sBasePath.find_last_of ('/');
         std::string sBaseDir   = (nLastSlash == std::string::npos) ? "/" : sBasePath.substr (0, nLastSlash + 1);

         sRefPath = sBaseDir + sReference;
      }

      sResult = sOrigin + RemoveDotSegments (sRefPath);
   }

   return sResult;
}

// ---------------------------------------------------------------------------
// WASM_FETCH — file-local helper that handles async .wasm module fetches.
// One instance per module declared in the MSF payload.
// ---------------------------------------------------------------------------

class WASM_FETCH : public IFILE
{
public:
   WASM_FETCH (FABRIC* pFabric, SCENE* pScene, const std::string& sUrl, const std::string& sHash) :
      m_pFabric (pFabric),
      m_pScene  (pScene),
      m_sUrl    (sUrl),
      m_sHash   (sHash),
      m_pFile   (nullptr)
   {
   }

   bool Initialize (CONTAINER* pContainer)
   {
      m_pFile = pContainer->Cache ()->File_Open (m_sUrl, m_sHash, 0, this);

      return (m_pFile != nullptr);
   }

   ~WASM_FETCH ()
   {
      if (m_pFile)
      {
         m_pFile->Close ();
         m_pFile = nullptr;
      }
   }

   void OnFileReady  (SNEEZE::FILE* pFile) override { m_pFabric->OnWasmReady  (pFile, m_sUrl, m_sHash); delete this; }
   void OnFileFailed (SNEEZE::FILE* pFile) override { m_pFabric->OnWasmFailed (pFile, m_sUrl         ); delete this; }

   FABRIC*        m_pFabric;
   SCENE*         m_pScene;
   std::string    m_sUrl;
   std::string    m_sHash;
   SNEEZE::FILE*  m_pFile;
};


// ---------------------------------------------------------------------------
// FABRIC::Impl
// ---------------------------------------------------------------------------

class FABRIC::Impl
{
public:
   Impl (FABRIC* pFabric, SCENE* pScene, CONTAINER* pContainer, uint64_t twFabricIx, NODE* pNode_Attach, MSF* pMsf) :
      m_pFabric         (pFabric),
      m_pScene          (pScene),
      m_pContainer      (pContainer),
      m_twFabricIx      (twFabricIx),
      m_pNode_Attach    (pNode_Attach),
      m_pMsf            (pMsf),
      m_pFabric_Parent  (pNode_Attach ? pNode_Attach->Fabric () : nullptr),
      m_pNode_Root      (nullptr),
      m_tmPerfOrigin    (0),
      m_ft100PerfOrigin (0)
   {
      // Anchor the PERFORMANCE origin at the fabric's onset (its timeOrigin).
      DEP::Performance_Origin_Capture (m_tmPerfOrigin, m_ft100PerfOrigin);

      if (m_pNode_Attach)
         m_pNode_Attach->Fabric_Add (m_pFabric);
   }

   bool Initialize (const std::string& sUrl)
   {
      bool bResult = true;

      m_sUrl = sUrl;

      if (m_pMsf)
      {
         auto aModule = m_pMsf->Modules ();

         if (!aModule.empty ())
         {
            for (auto& Module : aModule)
            {
               WASM_FETCH* pWasm_Fetch = new WASM_FETCH (m_pFabric, m_pScene, ResolveUrl (m_sUrl, Module.sUrl), Module.sHash);

               m_apWasm_Fetch.push_back (pWasm_Fetch);

               pWasm_Fetch->Initialize (m_pContainer);
            }

            std::string sModMsg = "Fetching " + std::to_string (aModule.size ()) + " WASM module(s)";
            m_pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "FABRIC", sModMsg);
            m_pContainer->Stream ()->Info (sModMsg, true);
         }

         if (m_apWasm_Fetch.empty ())
            WasmFetch_Complete ();
      }

      return bResult;
   }

   ~Impl ()
   {
      for (auto* pWasm_Fetch : m_apWasm_Fetch)
         delete pWasm_Fetch;
      m_apWasm_Fetch.clear ();

      if (m_pNode_Root)
      {
         m_pContainer->Node_Close (m_pNode_Root->ObjectIx ());
         m_pNode_Root = nullptr;
      }

      if (!m_apFabric.empty ())
         m_pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "FABRIC", "Leaked " + std::to_string (m_apFabric.size ()) + " child fabric(s)");

      for (auto& pair : m_aModule)
         m_pContainer->Instance_Close (m_twFabricIx, pair.first, pair.second);
      m_aModule.clear ();

      if (m_pNode_Attach)
         m_pNode_Attach->Fabric_Remove (m_pFabric);
   }

// -----------------------------------------------------------------------
// Snapshot_Build — the immutable blob the engine pushes into guest memory
// at Open. One JSON document of fixed-shape sections — RESOURCE / CONTAINER /
// SIGNATURE / AGENT / MODULES — each parsed guest-side into a typed object. The
// payload's open-ended "Data" and its "Services" are not pushed here; they are
// served read-only on demand from the fabric's MSF payload, like storage. Object members are
// Proper Case, scalar leaves are Hungarian (mirroring the source structs).
// qwResource is a decimal string; eTrust is the eSNEEZE_ABI_TRUST integer; the
// LOCATION view splits the URL guest-side from Resource.sReference.
// -----------------------------------------------------------------------

   std::string Snapshot_Build () const
   {
      nlohmann::json jSnapshot = nlohmann::json::object ();

      // RESOURCE — the launching resource's identity (the attaching node's map
      // object), plus this fabric's URL as sReference. The primary fabric has no
      // attaching node, so qwResource/sName are empty there.
      {
         uint64_t    qwResource = 0;
         std::string sName;

         if (m_pNode_Attach  &&  m_pNode_Attach->Map_Object ())
         {
            const MAP_OBJECT::MAP_OBJECT_RESOURCE& Resource = m_pNode_Attach->Map_Object ()->Resource;

            qwResource = Resource.qwResource;
            sName.assign (Resource.sName, ::strnlen (Resource.sName, sizeof (Resource.sName)));
         }

         nlohmann::json jResource = nlohmann::json::object ();
         jResource["qwResource"] = std::to_string (qwResource);
         jResource["sName"]      = sName;
         jResource["sReference"] = m_sUrl;
         jSnapshot["Resource"]   = jResource;
      }

      // CONTAINER — the container identity (CID). Display names are composed
      // guest-side, not transported. The CID carries only the persona hash; the
      // persona name comes off the live PERSONA (the same object the hash came
      // from), reached through the engine.
      {
         const CONTAINER::CID* pCID     = m_pContainer->Identity ();
         persona::PERSONA*     pPersona = m_pScene->Engine ()->Persona ();

         nlohmann::json jContainer = nlohmann::json::object ();
         jContainer["sContainer"]        = pCID ? pCID->sContainer        : std::string ();
         jContainer["sOrganization"]     = pCID ? pCID->sOrganization     : std::string ();
         jContainer["sOrganizationHash"] = pCID ? pCID->sOrganizationHash : std::string ();
         jContainer["sPersona"]          = pPersona ? pPersona->Name ()   : std::string ();
         jContainer["sPersonaHash"]      = pCID ? pCID->sPersonaHash      : std::string ();
         jContainer["sFingerprint"]      = pCID ? pCID->sFingerprint      : std::string ();
         jContainer["eTrust"]            = static_cast<int> (pCID ? pCID->eTrust : kTRUST_NONE);
         jSnapshot["Container"]          = jContainer;
      }

      // SIGNATURE — the MSF verification result.
      {
         nlohmann::json jSignature = nlohmann::json::object ();
         jSignature["sAlgorithm"]      = m_pMsf ? m_pMsf->Algorithm ()        : std::string ();
         jSignature["bSignatureValid"] = m_pMsf ? m_pMsf->IsSignatureValid () : false;
         jSignature["bChainTrusted"]   = m_pMsf ? m_pMsf->IsChainTrusted ()   : false;
         jSignature["bChainExpired"]   = m_pMsf ? m_pMsf->IsChainExpired ()   : false;
         jSnapshot["Signature"]        = jSignature;
      }

      // AGENT — host-supplied identity (navigator analog). Placeholder values
      // until the host supplies the real browser/platform/locale; the engine
      // name/version are the engine's own.
      {
         nlohmann::json jAgent = nlohmann::json::object ();
         jAgent["sBrowser_Name"]    = "Unknown";
         jAgent["sBrowser_Version"] = "0.0.0";
         jAgent["sEngine_Name"]     = "Sneeze";
         jAgent["sEngine_Version"]  = "0.1.0";
         jAgent["sPlatform"]        = "Unknown";
         jAgent["sLanguage"]        = "en-US";
         jSnapshot["Agent"]         = jAgent;
      }

      // MODULES — the fabric's wasm modules (url/hash), lifted into a fixed-shape
      // array so the guest parses them as typed objects. Services are NOT pushed
      // here: they are served read-only and on-demand from the fabric's MSF
      // payload (keyed by service name), the same as the "Data" tree.
      {
         nlohmann::json jModules = nlohmann::json::array ();

         if (m_pMsf)
         {
            for (const MSF::MODULE& Module : m_pMsf->Modules ())
            {
               nlohmann::json jModule = nlohmann::json::object ();
               jModule["sUrl"]  = Module.sUrl;
               jModule["sHash"] = Module.sHash;
               jModules.push_back (jModule);
            }
         }

         jSnapshot["Modules"] = jModules;
      }

      return jSnapshot.dump ();
   }

// -----------------------------------------------------------------------
// WASM module fetched — compile and insert into container
// -----------------------------------------------------------------------

   void OnWasmReady (SNEEZE::FILE* pFile, const std::string& sUrl, const std::string& sHash)
   {
      std::vector<uint8_t> aData;

      pFile->ReadData (aData);

      if (!aData.empty ())
      {
         std::string          sSnapshot = Snapshot_Build ();
         std::vector<uint8_t> aSnapshot (sSnapshot.begin (), sSnapshot.end ());

         if (m_pContainer->Instance_Open (m_twFabricIx, sUrl, sHash, aData, aSnapshot))
         {
            m_aModule.push_back (std::make_pair (sUrl, sHash));

            std::string sWasmMsg = "Loaded WASM: " + sUrl;
            m_pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "FABRIC", sWasmMsg);
            m_pContainer->Stream ()->Info (sWasmMsg, true);
         }
         else
         {
            std::string sWasmErr = "Failed to load WASM: " + sUrl;
            m_pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "FABRIC", sWasmErr);
            m_pContainer->Stream ()->Error (sWasmErr, true);
         }
      }

      WasmFetch_Remove (sUrl);
   }

   void OnWasmFailed (SNEEZE::FILE* pFile, const std::string& sUrl)
   {
      std::string sFetchErr = "Failed to fetch WASM: " + sUrl;
      m_pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "FABRIC", sFetchErr);
      m_pContainer->Stream ()->Error (sFetchErr, true);

      WasmFetch_Remove (sUrl);
   }

   void WasmFetch_Remove (const std::string& sUrl)
   {
      for (auto it = m_apWasm_Fetch.begin (); it != m_apWasm_Fetch.end (); ++it)
      {
         if ((*it)->m_sUrl == sUrl)
         {
            m_apWasm_Fetch.erase (it);
            break;
         }
      }

      if (m_apWasm_Fetch.empty ())
         WasmFetch_Complete ();
   }

   void WasmFetch_Complete ()
   {
      if (!m_aModule.empty ())
      {
         std::string sActiveMsg = std::to_string (m_aModule.size ()) + " WASM instance(s) active";
         m_pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "FABRIC", sActiveMsg);
         m_pContainer->Stream ()->Info (sActiveMsg, true);
      }
   }

// -----------------------------------------------------------------------
// Called internally from child fabrics
// -----------------------------------------------------------------------

   void Fabric_Add (FABRIC* pFabric_Child)
   {
      std::lock_guard<std::recursive_mutex> lock (m_mxFabric);

      m_apFabric.push_back (pFabric_Child);
   }

   void Fabric_Remove (FABRIC* pFabric_Child)
   {
      std::lock_guard<std::recursive_mutex> lock (m_mxFabric);

      auto it = std::find (m_apFabric.begin (), m_apFabric.end (), pFabric_Child);
      if (it != m_apFabric.end ())
      {
         (*it)->m_pImpl->m_pFabric_Parent = nullptr;
         m_apFabric.erase (it);
      }
   }

public:
   SCENE*                                              m_pScene;
   FABRIC*                                             m_pFabric;
   FABRIC*                                             m_pFabric_Parent;
   std::vector<FABRIC*>                                m_apFabric;
   NODE*                                               m_pNode_Root;
   NODE*                                               m_pNode_Attach;
   CONTAINER*                                          m_pContainer;
   uint64_t                                            m_twFabricIx;
   MSF*                                                m_pMsf;
   std::string                                         m_sUrl;
   std::vector<WASM_FETCH*>                            m_apWasm_Fetch;
   std::vector<std::pair<std::string, std::string>>    m_aModule;
   mutable std::recursive_mutex                        m_mxFabric;
   int64_t                                             m_tmPerfOrigin;
   int64_t                                             m_ft100PerfOrigin;
};

// ---------------------------------------------------------------------------
// FABRIC
// ---------------------------------------------------------------------------

FABRIC::FABRIC (SCENE* pScene, CONTAINER* pContainer, uint64_t twFabricIx, NODE* pNode_Attach, MSF* pMsf) :
   m_pImpl (new Impl (this, pScene, pContainer, twFabricIx, pNode_Attach, pMsf))
{
}

bool FABRIC::Initialize (const std::string& sUrl)
{
   return m_pImpl->Initialize (sUrl);
}

FABRIC::~FABRIC ()
{
   delete m_pImpl;
   m_pImpl = nullptr;
}

// -----------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------

SCENE*             FABRIC::Scene                     ()                              const { return m_pImpl->m_pScene; }
CONTAINER*         FABRIC::Container                 ()                              const { return m_pImpl->m_pContainer; }
MSF*               FABRIC::Msf                       ()                              const { return m_pImpl->m_pMsf; }
uint64_t           FABRIC::FabricIx                  ()                              const { return m_pImpl->m_twFabricIx; }
FABRIC*            FABRIC::Fabric_Parent             ()                              const { return m_pImpl->m_pFabric_Parent; }
NODE*              FABRIC::Node_Root                 ()                              const { return m_pImpl->m_pNode_Root; }
NODE*              FABRIC::Node_Attach               ()                              const { return m_pImpl->m_pNode_Attach; }
const std::string& FABRIC::Url                       ()                              const { return m_pImpl->m_sUrl; }
std::string        FABRIC::Resolve                   (const std::string& sReference) const { return ResolveUrl (m_pImpl->m_sUrl, sReference); }
int64_t            FABRIC::Performance_Origin_Steady ()                              const { return m_pImpl->m_tmPerfOrigin; }
int64_t            FABRIC::Performance_Origin_Wall   ()                              const { return m_pImpl->m_ft100PerfOrigin; }

// -----------------------------------------------------------------------
// Mutators
// -----------------------------------------------------------------------

void               FABRIC::Node_Root      (NODE* pNode_Root)              {         m_pImpl->m_pNode_Root = pNode_Root; }

// -----------------------------------------------------------------------
// Called internally from child fabrics
// -----------------------------------------------------------------------

void               FABRIC::Fabric_Add     (FABRIC* pFabric_Child)         {         m_pImpl->Fabric_Add    (pFabric_Child); }
void               FABRIC::Fabric_Remove  (FABRIC* pFabric_Child)         {         m_pImpl->Fabric_Remove (pFabric_Child); }

// -----------------------------------------------------------------------
// Fetch callbacks (delegated from WASM_FETCH helpers)
// -----------------------------------------------------------------------

void               FABRIC::OnWasmReady   (SNEEZE::FILE* pFile, const std::string& sUrl, const std::string& sHash)   { m_pImpl->OnWasmReady  (pFile, sUrl, sHash); }
void               FABRIC::OnWasmFailed  (SNEEZE::FILE* pFile, const std::string& sUrl)                             { m_pImpl->OnWasmFailed (pFile, sUrl); } 
