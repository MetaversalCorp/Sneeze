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

#ifndef SNEEZE_NETWORK_INETWORKIMPL_H
#define SNEEZE_NETWORK_INETWORKIMPL_H

#include "sneeze/control/Control.h"

namespace SNEEZE
{
   class INETWORK_IMPL
   {
   public:
      INETWORK_IMPL ();
      virtual ~INETWORK_IMPL ();

      virtual ASSET*             Asset_Open   (FILE* pFile)                                                                = 0;
      virtual void               Asset_Close  (FILE* pFile, ASSET* pAsset)                                                 = 0;
      virtual uint32_t           Asset_Index  ()                                                                           = 0;

      virtual std::string        Reset_Stale (const std::string& sKey) const                                               = 0;
      virtual void               Queue_Post_Fetch (JOB_FETCH* pJob_Fetch)                                                  = 0;

      virtual void               Log (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage)   = 0;

      virtual double             SecondsSinceEpoch () const                                                                = 0;


   private:
   };

   // -----------------------------------------------------------------------
   // ICACHE_IMPL - the single owner a FILE talks to.
   //
   // A FILE reaches everything it needs through this interface: its file
   // lifecycle (File_Clear / File_Close / File_Reset) lives in CACHE, while
   // asset operations, the host, the permanent cache path, and the owning
   // container are forwarded by CACHE to the shared NETWORK.
   // -----------------------------------------------------------------------

   class ICACHE_IMPL
   {
   public:
      ICACHE_IMPL ();
      virtual ~ICACHE_IMPL ();

      virtual ASSET*             Asset_Open  (FILE* pFile)                = 0;
      virtual void               Asset_Close (FILE* pFile, ASSET* pAsset) = 0;
      virtual uint32_t           Asset_Index ()                           = 0;

      virtual void               File_Clear  (FILE* pFile)                = 0;
      virtual void               File_Close  (FILE* pFile)                = 0;
      virtual void               File_Reset  (FILE* pFile)                = 0;

      virtual std::string        Reset_Stale () const                     = 0;

      virtual ICONTEXT*          Host        () const                     = 0;
      virtual std::string        Path        () const                     = 0;

      // Where an uncacheable response lives: the container's transitory tree,
      // scrubbed when the context closes and on the next engine start.
      virtual std::string        Path_Transitory () const                 = 0;
      virtual CONTAINER*         Container   () const                     = 0;

   private:
   };

   // -----------------------------------------------------------------------
   // ISOCKET_LINK - what a live connection reports back into its SOCKET.
   //
   // Implemented by SOCKET::Impl, called by SOCKET_HUB on the io thread. It
   // exists so the hub never reaches inside a SOCKET, which is what keeps every
   // websocketpp and asio type confined to Socket.cpp.
   // -----------------------------------------------------------------------

   class ISOCKET_LINK
   {
   public:
      ISOCKET_LINK ();
      virtual ~ISOCKET_LINK ();

      virtual void OnOpened  (const std::string& sProtocol)                     = 0;
      virtual void OnMessage (const uint8_t* pData, size_t nSize, bool bBinary) = 0;
      virtual void OnFailed  (const std::string& sError)                        = 0;
      virtual void OnClosed  (uint16_t wCode, bool bClean)                      = 0;
   };

   // -----------------------------------------------------------------------
   // SOCKET_HUB - the one asio io thread, and the websocketpp endpoints every
   // SOCKET shares.
   //
   // NETWORK creates it on the first Socket_Open and shuts it down on the way
   // out. Connections are keyed by their ISOCKET_LINK, so a socket names its
   // own connection with the same pointer it hears back on and the hub needs no
   // handle of its own.
   //
   // Everything below is safe to call from any thread. The callbacks all run on
   // the io thread.
   // -----------------------------------------------------------------------

   class SOCKET_HUB
   {
   public:
      SOCKET_HUB ();
      ~SOCKET_HUB ();

      bool Initialize ();

      // Starts the handshake for pLink. From here until Detach, pLink hears
      // about the connection on the io thread.
      bool Connect (ISOCKET_LINK* pLink, const std::string& sUrl, const std::string& sProtocol);

      bool Send    (ISOCKET_LINK* pLink, const uint8_t* pData, size_t nSize, bool bBinary);
      bool Close   (ISOCKET_LINK* pLink, uint16_t wCode, const std::string& sReason);

      uint64_t Buffered (ISOCKET_LINK* pLink) const;

      // Retires a connection. Once this returns, no callback for pLink is
      // running or ever will, so its SOCKET is safe to destroy.
      void Detach  (ISOCKET_LINK* pLink);

      // Stops the io thread. Detaches whatever is still connected first, so no
      // callback outlives the call.
      void Shutdown ();

   private:
      class Impl;
      Impl* m_pImpl;
   };

   // -----------------------------------------------------------------------
   // ASSET - internal shared state for a single cached URL.
   //
   // Owned by NETWORK, never exposed to callers directly. One ASSET per URL.
   // Multiple FILE handles may reference the same ASSET.
   // -----------------------------------------------------------------------

   class ASSET
   {
   public:
      ASSET (INETWORK_IMPL* m_pINetwork_Impl, const std::string& sUrl, const std::string& sPathname);
      virtual ~ASSET ();

      // Lifecycle
      void        Open   (FILE* pFile);
      uint32_t    Close  (FILE* pFile);

      bool        Attach (FILE* pFile, bool bFetch_Allowed, const std::string& sStaleAt);
      void        Detach (FILE* pFile);

      // Fetch completion (called by FETCH thread)
      void        Fetch_Complete (const FETCH_RESULT& Fetch_Result, eASSET_STATE bState);

      // Hash verification
      bool        VerifyHash (const std::string& sFilePath, const std::string& sHash) const;

      void ReadData (std::vector<uint8_t>& aData) const;
      void ReadRequestData (std::vector<uint8_t>& aData) const;
      std::string RspHeader (const std::string& sName) const;

      // Accessors
      eASSET_STATE         State             () const;
      bool                 IsReset           () const;
      size_t               File_Count        () const;
      const std::string&   Url               () const;
      uint64_t             SizeBytes         () const;
      std::string          CreatedTime       () const;
      std::string          LastAccessTime    () const;
      uint32_t             AccessCount       () const;
      uint32_t             AssetIx           () const;
      const std::string&   Hash              () const;
      bool                 IsHashed          () const;
      std::string          DiskPath          () const;
      const std::string&   Pathname          () const;
      std::string          Path              () const;
      std::string          Pathname          (eASSET_EXT eType) const;
      long                 HttpStatus        () const;
      double               FetchStartTime    () const;
      double               FetchEndTime      () const;
      double               FetchDuration     () const;
      double               FetchQueuedTime   () const;
      double               QueueDuration     () const;
      bool                 IsServedFromCache () const;
      eREQUEST_VERB        Verb              () const;
      uint64_t             RequestBytes      () const;
      const std::unordered_map<std::string, std::string>& RspHeaders () const;
      const std::unordered_map<std::string, std::string>& ReqHeaders () const;
      const std::string&   RemoteAddress     () const;
      const std::string&   Error             () const;

      // Modifiers
      void Reset              ();

   private:
      class Impl;
      Impl* m_pImpl;
   };
}
#endif // SNEEZE_NETWORK_INETWORKIMPL_H
