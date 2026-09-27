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

#ifndef SNEEZE_NETWORK_H
#define SNEEZE_NETWORK_H

namespace SNEEZE
{
   class ASSET;
   class INETWORK_IMPL;
   class ICACHE_IMPL;
   class CONTAINER;

   // ---------------------------------------------------------------------------
   // Network enums and interfaces
   // ---------------------------------------------------------------------------

   enum eASSET_STATE
   {
      kASSET_STATE_IDLE       = 0,
      kASSET_STATE_FETCHING   = 1,
      kASSET_STATE_VALIDATING = 2,
      kASSET_STATE_READY      = 3,
      kASSET_STATE_FAILED     = 4,
   };

   enum eASSET_EXT
   {
      kASSET_EXT_DATA    = 0,
      kASSET_EXT_TEMP    = 1,
      kASSET_EXT_META    = 2,
      kASSET_EXT_REQUEST = 3,
   };

   // ---------------------------------------------------------------------------
   // eREQUEST_VERB - the HTTP method a fetch uses.
   //
   // GET is 0 so it is the default for every caller that does not ask for
   // anything else. Only GET participates in the cache: every other verb is
   // fetched fresh, keyed uniquely, and stored in transitory space.
   // ---------------------------------------------------------------------------

   enum eREQUEST_VERB
   {
      kREQUEST_VERB_GET    = 0,
      kREQUEST_VERB_POST   = 1,
      kREQUEST_VERB_PUT    = 2,
      kREQUEST_VERB_PATCH  = 3,
      kREQUEST_VERB_DELETE = 4,
      kREQUEST_VERB_HEAD   = 5,
   };

   // Largest response a WASM guest may pull. Engine-driven asset fetches are
   // uncapped - a legitimate model is routinely tens of megabytes.
   const uint64_t kREQUEST_SIZE_MAX = 256ull * 1024ull * 1024ull;

   // ---------------------------------------------------------------------------
   // REQUEST - the optional attributes of a single fetch.
   //
   // Defaults reproduce the engine's original behavior exactly: a GET with no
   // caller headers, no body, the default timeout, no size cap, and a non-2xx
   // treated as a failure. A WASM guest overrides these; nothing else does.
   // ---------------------------------------------------------------------------

   struct REQUEST
   {
      eREQUEST_VERB                                  eVerb          { kREQUEST_VERB_GET };
      std::unordered_map<std::string, std::string>   umsHeader;
      std::vector<uint8_t>                           aBody;
      long                                           nTimeout_Milli { 0 };
      uint64_t                                       nSizeMax       { 0 };

      // XHR semantics: any HTTP response is a completed request, so a 404 keeps
      // its body and its status. False (the engine default) treats a non-2xx as
      // a failed fetch and discards what came back.
      bool                                           bAnyStatus     { false };
   };

   class FILE;

   class IFILE
   {
   public:
      virtual ~IFILE () {}
      virtual void OnFileReady  (FILE* pFile) = 0;
      virtual void OnFileFailed (FILE* pFile) = 0;
   };

   class IENUM_FILE
   {
   public:
      virtual ~IENUM_FILE () {}
      virtual void OnAsset (FILE* pFile) = 0;
   };

   // ---------------------------------------------------------------------------
   // FILE - per-caller handle to a cached resource.
   //
   // Created by NETWORK::File_Open(), returned to the caller as a raw pointer.
   // Owns a snapshot of the asset's display-level fields so the inspector can
   // read them after Close.
   // ---------------------------------------------------------------------------

   class FILE
   {
   public:
      FILE (ICACHE_IMPL* pICache_Impl, uint32_t nFileIx, const std::string& sUrl, const std::string& sHash, bool bCacheEnabled, const REQUEST& Request);
      ~FILE ();

      // --- Snapshot fields (always available, even after Close) ---

      eASSET_STATE     State             () const;
      bool             IsReady           () const;

      std::string      Url               () const;
      std::string      Hash              () const;
      bool             IsHashed          () const;

      eREQUEST_VERB    Verb              () const;
      bool             IsCacheable       () const;

      uint32_t         FileIx            () const;
      uint32_t         AssetIx           () const;
      long             HttpStatus        () const;
      double           FetchQueuedTime   () const;
      double           FetchStartTime    () const;
      double           FetchEndTime      () const;
      double           FetchDuration     () const;
      bool             IsServedFromCache () const;

      std::string      ContentType       () const;
      uint64_t         SizeBytes         () const;

      // Empty unless the transport itself failed (a timeout, a refused
      // connection, an exceeded size cap). An HTTP error is not a transport
      // error - that shows up as the status.
      std::string      Error             () const;

      // --- ASSET-dependent (require attached ASSET, empty/default after Close) ---

      void             ReadData          (std::vector<uint8_t>& aData) const;
      void             ReadRequestData   (std::vector<uint8_t>& aData) const;
//    std::string      Header (const std::string& sName) const;
      std::string      DiskPath          () const;
      std::string      CreatedTime       () const;
      std::string      LastAccessTime    () const;
      uint32_t         AccessCount       () const;
      const std::unordered_map<std::string, std::string>& ReqHeaders () const;
      const std::unordered_map<std::string, std::string>& RspHeaders () const;

      const std::string&   RemoteAddress     () const;

      // --- Container ---

      std::string ContainerName () const;

      // --- Paths ---

      std::string        Path () const;
      std::string        Filename (const std::string& sExt = "") const;
      std::string        Pathname (const std::string& sExt = "") const;

      // --- Listener ---

      IFILE* Listener () const;

      // --- Open-time state (locked in at construction) ---

      const std::string& OpenHash  () const;
      bool               CacheEnabled () const;
      const REQUEST&     Request      () const;

      // --- Lifecycle ---

      bool   Initialize (IFILE* pListener = nullptr);
      bool   Attach     ();
      void   Detach     ();
      bool   Guard      (bool bValue);
      void   Clear      ();
      void   Close      ();
      void   Reset      ();

      // --- Internal (NETWORK use only) ---

      bool   IsPending_Clear () const;
      bool   IsPending_Close () const;

      bool   Close_Guarded ();

      bool   Pending_Clear ();
      bool   Pending_Close ();
      void   Pending_Reset ();

      void   Notify_Changed ();

      void   SnapshotInitial ();
      void   SnapshotProgress ();
      void   SnapshotFinal ();

   private:
      class Impl;
      Impl* m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // CACHE - per-container handle to the network's file tier.
   //
   // Opened from NETWORK::Cache_Open() for a specific CONTAINER and held by
   // that container for its lifetime. Owns the container's FILE handles and
   // exposes File_Open(). Files persist across restarts; files with a
   // cryptographic hash are additionally integrity-verified.
   //
   // The underlying deduplicated ASSET store (the disk cache itself) is owned
   // by NETWORK and shared across every CACHE - a CACHE forwards asset
   // operations to its NETWORK and contributes only the file-handle layer.
   // ---------------------------------------------------------------------------

   class CACHE
   {
   public:

      CACHE (INETWORK_IMPL* pINetwork_Impl, CONTAINER* pContainer);
      ~CACHE ();

      void  Initialize ();

      // --- Identity ---

      std::string DisplayName () const;

      // --- File operations ---

      FILE* File_Open (const std::string& sUrl, IFILE* pListener);
      FILE* File_Open (const std::string& sUrl, const std::string& sHash, uint32_t nAssetIx = 0, IFILE* pListener = nullptr);
      FILE* File_Open (const std::string& sUrl, const std::string& sHash, const REQUEST& Request, IFILE* pListener);

      void  File_Enum (IENUM_FILE* pEnum);

      // --- Cache management ---

      void  SetCacheEnabled (bool b);
      bool  IsCacheEnabled () const;

      void  Clear ();

      // --- Paths ---

      std::string Path     () const;
      std::string Filename (const std::string& sExt = "") const;
      std::string Pathname (const std::string& sExt = "") const;

   private:
      class Impl;
      Impl* m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // IENUM_CACHE - enumeration callback interface (caches).
   // ---------------------------------------------------------------------------

   class IENUM_CACHE
   {
   public:
      virtual ~IENUM_CACHE () {}
      virtual void OnCache (CACHE* pCache) = 0;
   };

   class SOCKET;
   class SOCKET_HUB;

   // ---------------------------------------------------------------------------
   // eSOCKET_STATE - mirrors WebSocket.readyState, values included.
   // ---------------------------------------------------------------------------

   enum eSOCKET_STATE
   {
      kSOCKET_STATE_CONNECTING = 0,
      kSOCKET_STATE_OPEN       = 1,
      kSOCKET_STATE_CLOSING    = 2,
      kSOCKET_STATE_CLOSED     = 3,
   };

   // Largest single frame a socket will accept, in either direction. The same
   // ceiling the guest's requests get: a conversation that needs more than this
   // per message wants a fetch, not a socket.
   const uint64_t kSOCKET_FRAME_MAX = 16ull * 1024ull * 1024ull;

   // ---------------------------------------------------------------------------
   // ISOCKET - what a socket reports to whoever opened it.
   //
   // Every one of these arrives on the network's io thread, never on the thread
   // that opened the socket, and pData is only valid for the duration of the
   // call. An implementation hands the news off; it does not work in place.
   // ---------------------------------------------------------------------------

   class ISOCKET
   {
   public:
      virtual ~ISOCKET () {}
      virtual void OnSocketOpened  (SOCKET* pSocket)                                              = 0;
      virtual void OnSocketMessage (SOCKET* pSocket, const uint8_t* pData, size_t nSize, bool bBinary) = 0;
      virtual void OnSocketFailed  (SOCKET* pSocket)                                              = 0;
      virtual void OnSocketClosed  (SOCKET* pSocket, uint16_t wCode, bool bClean)                 = 0;
   };

   // ---------------------------------------------------------------------------
   // IENUM_SOCKET - enumeration callback interface (sockets).
   // ---------------------------------------------------------------------------

   class IENUM_SOCKET
   {
   public:
      virtual ~IENUM_SOCKET () {}
      virtual void OnSocket (SOCKET* pSocket) = 0;
   };

   // ---------------------------------------------------------------------------
   // SOCKET - one WebSocket connection, shaped like the browser's.
   //
   // Opened from NETWORK::Socket_Open() for a container and handed back as a raw
   // pointer, the same arrangement a FILE has. Connecting is asynchronous: the
   // socket returns CONNECTING and reports OnSocketOpened or OnSocketFailed once
   // the handshake settles.
   //
   // A socket is a live conversation rather than a resource, so unlike a FILE it
   // has no ASSET behind it and nothing it carries is cached or written to disk.
   // ---------------------------------------------------------------------------

   class SOCKET
   {
   public:
      SOCKET (SOCKET_HUB* pHub, CONTAINER* pContainer, uint32_t nSocketIx, const std::string& sUrl, const std::string& sProtocol);
      ~SOCKET ();

      // Begins the handshake. The listener starts hearing about the socket here.
      bool Initialize (ISOCKET* pListener);

      // --- Sending ---

      // Both refuse anything but an OPEN socket, and refuse a frame over
      // kSOCKET_FRAME_MAX.
      bool Send_Text   (const std::string& sText);
      bool Send_Binary (const uint8_t* pData, size_t nSize);

      // --- Closing ---

      // The closing handshake. 1000 is a normal closure; the protocol caps the
      // reason at 123 bytes and a longer one is truncated. OnSocketClosed still
      // follows, so a caller learns the close completed the same way either
      // side initiating it does.
      void Close (uint16_t wCode, const std::string& sReason);

      // --- State ---

      eSOCKET_STATE      State    () const;

      // Bytes handed to the socket that have not reached the wire yet
      // (bufferedAmount).
      uint64_t           Buffered () const;

      const std::string& Url      () const;

      // The subprotocol the server chose. Empty until the socket opens, and
      // empty after that if none was negotiated.
      std::string        Protocol () const;

      // Why the socket failed, when it did. Empty otherwise - a clean close is
      // not an error.
      std::string        Error    () const;

      // --- Identity ---

      uint32_t           SocketIx  () const;
      CONTAINER*         Container () const;
      ISOCKET*           Listener  () const;

      std::string        ContainerName () const;

   private:
      class Impl;
      Impl* m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // NETWORK - the network resource system.
   //
   // Fetches remote resources, caches them on disk, and serves them to callers
   // through per-container CACHE handles. Owns the deduplicated ASSET store and
   // the background fetch machinery. Each CONTAINER opens a CACHE via
   // Cache_Open() and returns it via Cache_Close().
   //
   // Assets are loaded lazily on first File_Open() (on a CACHE). Only assets
   // with active FILE handles live in memory; the .meta sidecar is flushed to
   // disk when the last active handle closes.
   //
   // Background fetches are capped at 16 concurrent threads. Overflow fetches
   // queue and are dispatched as threads complete.
   // ---------------------------------------------------------------------------

   class NETWORK
   {
   public:

      // -----------------------------------------------------------------------
      // NETWORK public API
      // -----------------------------------------------------------------------

      explicit NETWORK (ENGINE* pEngine);
      ~NETWORK ();

      bool Initialize (const std::string& sPath_Root);

      // --- Cache management ---

      CACHE* Cache_Open  (CONTAINER* pContainer);
      void   Cache_Close (CONTAINER* pContainer, CACHE* pCache);
      void   Cache_Enum  (IENUM_CACHE* pEnum);

      // --- Sockets ---

      // Opens a ws:// or wss:// connection for a container. sProtocol is the
      // comma-separated subprotocol list to offer, or empty to offer none.
      // Returns a CONNECTING socket, or null if the URL is not a WebSocket URL.
      //
      // The first call starts the shared io thread, so a session that never
      // opens a socket never pays for one.
      SOCKET* Socket_Open  (CONTAINER* pContainer, const std::string& sUrl, const std::string& sProtocol, ISOCKET* pListener);

      // Retires a socket: closes it if it is still up, then destroys it. No
      // listener callback arrives after this returns, and the pointer is dead.
      void    Socket_Close (SOCKET* pSocket);

      void    Socket_Enum  (IENUM_SOCKET* pEnum);

      // --- Reset ---

      void        Reset       (const std::string& sKey);
      std::string Time_Start  () const;

   private:
      class Impl;
      Impl* m_pImpl;
   };
}
#endif // SNEEZE_NETWORK_H
