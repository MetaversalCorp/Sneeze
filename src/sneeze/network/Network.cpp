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

#include "Network.h"

#include <ctime>
#include <iomanip>

#define RESERVE_BLOCK  1000

// JSON keys for the engine-wide "network_reset.json" record (read in Reset_Load,
// written in Reset_Save).
#define RESET_KEY_ASSET_IX_NEXT "nAssetIx_Next"
#define RESET_KEY_TIME_STALE    "sTime_Stale"
#define RESET_KEY_RESETS        "resets"

using namespace SNEEZE;

// Validates a stored timestamp is a well-formed ISO-8601 instant in the exact
// format NowIso8601 emits. Anything else (empty, garbage, partial) is rejected.

static bool IsIso8601 (const std::string& sTime)
{
   std::tm tm = {};
   std::istringstream ss (sTime);

   ss >> std::get_time (&tm, "%Y-%m-%dT%H:%M:%SZ");

   return !ss.fail ()  &&  ss.peek () == std::char_traits<char>::eof ();
}

/***********************************************************************************************************************************
**  Impl Class
***********************************************************************************************************************************/

#define RMAPTEST_MAP

#ifdef RMAPTEST_MAP
class ILOGGERApp : public RMAP::CORE::ILOGGER
{
public:
   ILOGGERApp ()
   {
   }

   ~ILOGGERApp ()
   {
   }

   void onMessage (std::string& sLine) override
   {
      std::printf ("%s", sLine.c_str ());
   }

private:
};


#define RMAP_STATE_NOTREADY                  0
#define RMAP_STATE_LOGGINGIN_AUTHENTICATE    1
#define RMAP_STATE_READY                     2

class RMAP_FABRIC : public RMAP::CORE::NOTIFICATION, public RMAP::CORE::IRESPONSE
{
public:
   RMAP::CORE::LNG* m_pLnG;

   RMAP_FABRIC ()
   {
      RMAP::CORE::APP* pCore = RMAP::CORE::APP::GetInstance ();

      m_pLnG = pCore->LnG_Open ("metaversal/rp1_map_earth", "Socket.IO", "secure=true;server=prod-map-earth.rp1.com:443;session=Null", "");
      m_pLnG->Attach (this);
   }

   ~RMAP_FABRIC ()
   {
   }

   void onReadyState (RMAP::CORE::INOTICE* pNotice)
   {
      bool bCheckSvc = true;

      if (pNotice->pCreator == m_pLnG)
      {
         switch (m_pLnG->ReadyState ())
         {
         case RMAP::CORE::LNG::eSTATE::DISCONNECTED: // UI Update: Disconnected 
            m_pLnG->Logout ();

            bCheckSvc = false;
            ReadyState (RMAP_STATE_NOTREADY);
            break;

         case RMAP::CORE::LNG::eSTATE::LOGGEDOUT:  // UI Update: Not Logged In
            m_pLnG->Logout ();

            bCheckSvc = true;
            break;

         case RMAP::CORE::LNG::eSTATE::LOGGEDIN:
            bCheckSvc = true;
            break;
         }
      }
      else if (m_pLnG)
      {
         if (pNotice->pCreator == m_pLnG->pSession () && m_pLnG->pSession ()->ReadyState () == RMAP::CORE::MODEL_SESSION_C2A::eSTATE::LOGGINGIN_AUTHENTICATE)
         {
            ReadyState (RMAP_STATE_LOGGINGIN_AUTHENTICATE);
         }
      }

      if (bCheckSvc)
      {
         bCheckSvc = false;

         if (m_pLnG->IsReady ())
         {
            ReadyState (RMAP_STATE_READY);
         }
      }
   }

   void Notify (RMAP::CORE::INOTICE* pNotice)
   {
      if (pNotice->sNotification.compare ("onReadyState") == 0)
         onReadyState (pNotice);
   }

   void onResponse (RMAP::CORE::CLIENT::IACTION* pIAction, int nType, intptr_t pParam)
   {
#if 0
      MV::MVSB::CLIENT::IACTION* pIActionSB = dynamic_cast<MV::MVSB::CLIENT::IACTION*> (pIAction);
      FABRIC_SERVICE* pFabric_Svc = reinterpret_cast<FABRIC_SERVICE*>(pParam);

      if (pIActionSB->GetResult () == 0)
      {
         ordered_json& jResponse = pIActionSB->GetResponse ();

         pFabric_Svc->pLnG->Login ("token = " + MV::MVMF::Escape (jResponse["acToken64U_RP1"]));
      }
      else
      {
         // TODO: Handle Error
      }

      delete pFabric_Svc;
#endif
   }
};

static RMAP_FABRIC* g_pFabric = NULL;
RMAP::CORE::ILOGGER* pLogger = new ILOGGERApp ();
#endif

INETWORK_IMPL::INETWORK_IMPL ()  {}
INETWORK_IMPL::~INETWORK_IMPL () {}

class NETWORK::Impl : public INETWORK_IMPL
{
public:
   Impl (NETWORK* pNetwork, ENGINE* pEngine) :
      INETWORK_IMPL      (),
      m_pNetwork         (pNetwork),
      m_pEngine          (pEngine),
      m_nAssetIx_Next    (1),
      m_nAssetIx_Reserve (1)
   {
   }

   bool Initialize (const std::string& sPath_Root)
   {
      bool bResult = false;

      m_tpEpoch     = std::chrono::steady_clock::now ();
      m_sTime_Start = NowIso8601 ();

      m_sPathname_Reset = (std::filesystem::path (sPath_Root) / "network_reset.json").generic_string ();

#ifdef RMAPTEST_MAP
      RMAP::CORE::APP* pCore = RMAP::CORE::APP::GetInstance ();
      RMAP::CORE::APP::REQUIRE* pRequire;   // TODO: Free this Require

      pCore->LoggerStart (pLogger);
#endif

      RMAP::CORE::Install ();
      RMAP::SVC_SB::Install ();
      RMAP::SVC_REST::Install ();
      RMAP::SVC_SOCKETIO::Install ();
      RMAP::MAP::Install ();


#ifdef RMAPTEST_MAP
      if ((pRequire = pCore->Require ("Map", "Socket.IO", "metaversal/rp1_map_earth")) != NULL)
      {
         g_pFabric = new RMAP_FABRIC ();
      }
#endif

      Reset_Load ();

      bResult = true;

      m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "NETWORK", "Initialized (reset: " + m_sPathname_Reset + ", resets: " + std::to_string (m_umsReset.size ()) + ", nAssetIx_Next: " + std::to_string (m_nAssetIx_Next) + ")");

      return bResult;
   }

   ~Impl ()
   {
      m_nAssetIx_Reserve = m_nAssetIx_Next; // persist the exact next index so a clean shutdown wastes no reserve

      Reset_Save ();

      {
         std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Cache);

         // Engine-level teardown: every CONTAINER has already closed its CACHE,
         // so this is a leak safety net. The owning contexts/containers are gone,
         // so no OnNetworkCacheDeleted callback can be routed — delete directly.

         for (auto* pCache : m_apCache)
            delete pCache;

         m_apCache.clear ();
      }

      // See note below
      //
      // for (auto& [sUrl, pAsset] : m_umpAsset)
      // {
      //    m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "NETWORK", "Leaked ASSET: " + pAsset->Url ());
      //    delete pAsset;
      // }
      // m_umpAsset.clear ();

      // There is a race condition in which fetch jobs in flight will set m_pAsset_Fetch to nullptr before
      // an asset being deleted has a chance to close it. There is no other known way to handle this, other 
      // than to wait for the assets to drain naturally. If all [leaked or otherwise] files have been deleted, 
      // then all of the assets should eventually drain unless there is a bug in the asset code.

      size_t nSize;
      do
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(1));

         {
            std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Asset);

            nSize = m_umpAsset.size ();
         }
      }
      while (nSize > 0);

      RMAP::CORE::Unstall ();
      RMAP::SVC_SB::Unstall ();
      RMAP::SVC_REST::Unstall ();
      RMAP::SVC_SOCKETIO::Unstall ();
      RMAP::MAP::Unstall ();
   }

   // ---------------------------------------------------------------------------
   // Reset management
   // ---------------------------------------------------------------------------

   void Reset_Load ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Reset);

      bool bValid = true;
            
      std::ifstream file (m_sPathname_Reset);
      if (file.is_open ())
      {
         try
         {
            nlohmann::json jReset;
            file >> jReset;

            m_nAssetIx_Next = jReset.value (RESET_KEY_ASSET_IX_NEXT, static_cast<uint32_t> (0));
            m_sTime_Stale   = jReset.at (RESET_KEY_TIME_STALE).get<std::string> ();

            bValid &= (m_nAssetIx_Next > 0);
            bValid &= IsIso8601 (m_sTime_Stale);

            for (auto& [sKey, jStale] : jReset.at (RESET_KEY_RESETS).items ())
            {
               std::string sStale = jStale.get<std::string> ();

               m_umsReset[sKey] = sStale;

               bValid &= IsIso8601 (sStale);
            }

            m_nAssetIx_Reserve = m_nAssetIx_Next;
         }
         catch (...)
         {
            bValid = false;  // any glitch -- parse, map, or value -- forces the failure path
         }
      }
      else bValid = false;

      if (!bValid)
      {
         // Missing, unparseable, or incomplete -- the counter cannot be trusted. Restart it and
         // stale every asset created before this session so reused indices cannot pass as fresh.

         m_nAssetIx_Reserve = m_nAssetIx_Next = 1;
         m_umsReset.clear ();
         m_sTime_Stale = NowIso8601 ();

         m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "NETWORK", "network_reset.json not loaded -- starting fresh");
      }
   }

   void Reset_Save ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Reset);

      nlohmann::json jReset;
      jReset[RESET_KEY_ASSET_IX_NEXT] = m_nAssetIx_Reserve;
      jReset[RESET_KEY_TIME_STALE]    = m_sTime_Stale;

      nlohmann::json jResets = nlohmann::json::object ();
      for (auto& [sKey, sStale] : m_umsReset)
         jResets[sKey] = sStale;
      jReset[RESET_KEY_RESETS] = jResets;

      std::string sPathname_Temp = m_sPathname_Reset + ".temp";

      std::ofstream file (sPathname_Temp, std::ios::trunc);
      if (file.is_open ())
      {
         file << jReset.dump (2);
         file.close ();

         std::error_code ec;
         std::filesystem::rename (sPathname_Temp, m_sPathname_Reset, ec);
      }
   }

   std::string Reset_Stale (const std::string& sKey) const override
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Reset);

      std::string sResult = m_sTime_Stale;

      auto it = m_umsReset.find (sKey);
      if (it != m_umsReset.end ()  &&  it->second > sResult)
         sResult = it->second;

      return sResult;
   }

   void Reset (const std::string& sKey)
   {
      if (!sKey.empty ())
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Reset);

         m_umsReset[sKey] = NowIso8601 ();

         Reset_Save ();
      }
   }

   // ---------------------------------------------------------------------------
   // Cache management
   // ---------------------------------------------------------------------------

   CACHE* Cache_Open (CONTAINER* pContainer)
   {
      CACHE* pCache = nullptr;

      if (pContainer)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Cache);

         pCache = new CACHE (this, pContainer);

         m_apCache.push_back (pCache);

         pCache->Initialize ();

         pContainer->Context ()->Host ()->OnNetworkCacheCreated (pCache);
      }

      return pCache;
   }

   void Cache_Close (CONTAINER* pContainer, CACHE* pCache)
   {
      if (pCache)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Cache);

         pContainer->Context ()->Host ()->OnNetworkCacheDeleted (pCache);

         auto it = std::find (m_apCache.begin (), m_apCache.end (), pCache);
         if (it != m_apCache.end ())
            m_apCache.erase (it);
            
         delete pCache;
      }
   }

   void Cache_Enum (IENUM_CACHE* pEnum)
   {
      if (pEnum)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Cache);

         for (CACHE* pCache : m_apCache)
            pEnum->OnCache (pCache);
      }
   }

   // ---------------------------------------------------------------------------
   // Timing helpers
   // ---------------------------------------------------------------------------

   double SecondsSinceEpoch () const override
   {
      auto tpNow = std::chrono::steady_clock::now ();

      return std::chrono::duration<double> (tpNow - m_tpEpoch).count ();
   }

   // ---------------------------------------------------------------------------
   // INETWORK_IMPL
   // ---------------------------------------------------------------------------

   ASSET* Asset_Open (FILE* pFile) override
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Asset);

      ASSET* pAsset = nullptr;

      std::string sUrl      = pFile->Url ();
      std::string sPathname = pFile->Pathname ("");

      auto it = m_umpAsset.find (sPathname);
      if (it == m_umpAsset.end ())
      {
         pAsset = new ASSET (this, sUrl, sPathname);

         m_umpAsset[sPathname] = pAsset;
      }
      else pAsset = it->second;

      pAsset->Open (pFile);

      return pAsset;
   }

   void Asset_Close (FILE* pFile, ASSET* pAsset) override
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Asset);

      if (pAsset->Close (pFile) == 0)
      {
         m_umpAsset.erase (pAsset->Pathname ()); // should be the same as pFile->Pathname ()

         delete pAsset;
      }
   }

   uint32_t Asset_Index () override
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxNetwork_Reset); // yes, m_mxNetwork_Reset (not m_mxNetwork_Asset)

      if (m_nAssetIx_Reserve <= m_nAssetIx_Next)
      {
         m_nAssetIx_Reserve = m_nAssetIx_Next + RESERVE_BLOCK;

         Reset_Save ();
      }

      return m_nAssetIx_Next++;
   }

   void Queue_Post_Fetch (JOB_FETCH* pJob_Fetch) override
   { 
      m_pEngine->Queue_Post_Fetch (pJob_Fetch); 
   }

   void Log (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage) override
   {
      m_pEngine->Log (Level, sModule, sMessage);
   }

   // ---------------------------------------------------------------------------

   NETWORK*                                     m_pNetwork;
   ENGINE*                                       m_pEngine;

   uint32_t                                     m_nAssetIx_Next;
   uint32_t                                     m_nAssetIx_Reserve;
   std::string                                  m_sPathname_Reset;
   std::string                                  m_sTime_Stale;
   std::string                                  m_sTime_Start;

   std::chrono::steady_clock::time_point        m_tpEpoch;

   std::unordered_map<std::string, std::string> m_umsReset;
   std::vector<CACHE*>                          m_apCache;
   std::unordered_map<std::string, ASSET*>      m_umpAsset;

   mutable std::recursive_mutex                 m_mxNetwork_Reset;
   mutable std::recursive_mutex                 m_mxNetwork_Cache;
   mutable std::recursive_mutex                 m_mxNetwork_Asset;
};

// ---------------------------------------------------------------------------
// NETWORK
// ---------------------------------------------------------------------------

NETWORK::NETWORK (ENGINE* pEngine) :
   m_pImpl (new Impl (this, pEngine))
{
}

bool NETWORK::Initialize (const std::string& sPath_Root)
{
   return m_pImpl->Initialize (sPath_Root);
}

NETWORK::~NETWORK ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

CACHE*      NETWORK::Cache_Open  (CONTAINER* pContainer)                { return m_pImpl->Cache_Open  (pContainer); }
void        NETWORK::Cache_Close (CONTAINER* pContainer, CACHE* pCache) {        m_pImpl->Cache_Close (pContainer, pCache); }
void        NETWORK::Cache_Enum  (IENUM_CACHE* pEnum)                   {        m_pImpl->Cache_Enum  (pEnum); }

void        NETWORK::Reset       (const std::string& sKey)              {        m_pImpl->Reset (sKey); }

std::string NETWORK::Time_Start  () const                               { return m_pImpl->m_sTime_Start; }
