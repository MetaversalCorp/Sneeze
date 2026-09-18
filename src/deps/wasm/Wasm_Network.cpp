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

#include "Wasm.h"

#include <sneeze_abi.h>

using namespace SNEEZE::DEP;

// A guest that stops draining its receive queue must not be able to grow the
// engine's memory without bound. Past this many bytes the socket keeps running
// but arriving messages are dropped and Socket_Error says so. Closing the
// connection instead is not available: that would mean calling into the socket
// from inside one of its own callbacks.
#define SOCKET_QUEUE_MAX (16ull * 1024ull * 1024ull)

// ===========================================================================
// WASM_NETWORK::LISTENER
// ===========================================================================

WASM_NETWORK::LISTENER::LISTENER (WASM_NETWORK* pNetwork, uint64_t twRequestIx) :
   m_pNetwork    (pNetwork),
   m_twRequestIx (twRequestIx),
   m_bOrphan     (false)
{
}

// ---------------------------------------------------------------------------
// Orphan - the request this listener was serving has been dropped while its
// fetch was still running. The fetch cannot be recalled, so the listener stays
// live to absorb the completion; on arrival it closes the FILE (nobody else
// will) and reports nothing to a guest.
// ---------------------------------------------------------------------------

void WASM_NETWORK::LISTENER::Orphan ()
{
   m_bOrphan.store (true, std::memory_order_release);
}

void WASM_NETWORK::LISTENER::OnFileReady  (SNEEZE::FILE* pFile) { Retire (pFile, true);  }
void WASM_NETWORK::LISTENER::OnFileFailed (SNEEZE::FILE* pFile) { Retire (pFile, false); }

// ---------------------------------------------------------------------------
// Retire - the one and only exit. A FILE calls exactly one of ready/failed per
// fetch, so this runs exactly once and the listener deletes itself here.
//
// The FILE is closed here whenever no live request took the result - either the
// listener was orphaned before it fired, or it was orphaned in the instant
// between reading the flag and reaching the table. Request_Complete reporting
// whether it claimed the result is what closes that window: exactly one of the
// two sides closes the FILE, whichever order they happen to run in.
//
// Closing from inside the callback is the sanctioned pattern: ASSET holds its
// guard across the whole completion, so the close is deferred and re-issued by
// the completion itself once the guard drops.
// ---------------------------------------------------------------------------

void WASM_NETWORK::LISTENER::Retire (SNEEZE::FILE* pFile, bool bSuccess)
{
   bool bClaimed = false;

   if (!m_bOrphan.load (std::memory_order_acquire))
      bClaimed = m_pNetwork->Request_Complete (m_twRequestIx, pFile, bSuccess);

   if (!bClaimed)
      pFile->Close ();

   delete this;
}

// ===========================================================================
// WASM_NETWORK::SOCKET_LISTENER
// ===========================================================================

WASM_NETWORK::SOCKET_LISTENER::SOCKET_LISTENER (WASM_NETWORK* pNetwork, uint64_t twSocketIx) :
   m_pNetwork   (pNetwork),
   m_twSocketIx (twSocketIx)
{
}

// ---------------------------------------------------------------------------
// All four arrive on the network's io thread. Reading State/Protocol/Error back
// off the socket here is allowed; Send, Close and Buffered are not (see ISOCKET
// in Network.h). Nothing below needs them.
// ---------------------------------------------------------------------------

void WASM_NETWORK::SOCKET_LISTENER::OnSocketOpened (SNEEZE::SOCKET* pSocket)
{
   m_pNetwork->Socket_Opened (m_twSocketIx, pSocket->Protocol ());
}

void WASM_NETWORK::SOCKET_LISTENER::OnSocketMessage (SNEEZE::SOCKET* pSocket, const uint8_t* pData, size_t nSize, bool bBinary)
{
   (void) pSocket;

   m_pNetwork->Socket_Message (m_twSocketIx, pData, nSize, bBinary);
}

void WASM_NETWORK::SOCKET_LISTENER::OnSocketFailed (SNEEZE::SOCKET* pSocket)
{
   m_pNetwork->Socket_Failed (m_twSocketIx, pSocket->Error ());
}

void WASM_NETWORK::SOCKET_LISTENER::OnSocketClosed (SNEEZE::SOCKET* pSocket, uint16_t wCode, bool bClean)
{
   (void) pSocket;

   m_pNetwork->Socket_Closed (m_twSocketIx, wCode, bClean);
}

// ===========================================================================
// WASM_NETWORK
// ===========================================================================

WASM_NETWORK::WASM_NETWORK (ENGINE* pEngine) :
   m_pEngine        (pEngine),
   m_twRequest_Next (1),
   m_twSocket_Next  (1),
   m_nInFlight      (0)
{
}

// ---------------------------------------------------------------------------
// Engine teardown. Every store has already been closed through Store_Close, so
// the tables are expected to be empty; a surviving entry means a store leaked
// past its own close and its listener is orphaned rather than deleted (the
// fetch that owns it may still be running).
// ---------------------------------------------------------------------------

WASM_NETWORK::~WASM_NETWORK ()
{
   std::lock_guard<std::mutex> guard (m_mxNetwork);

   for (size_t i = 0; i < m_aEntry.size (); i++)
   {
      if (m_aEntry[i].pListener)
         m_aEntry[i].pListener->Orphan ();
   }

   // A surviving socket is the same kind of leak, with one difference: the
   // engine deletes NETWORK before it deletes the runtime that owns this, so
   // every SOCKET here is already gone and pSocket must not be touched. What is
   // left to release is the listener, and NETWORK's own teardown (which deleted
   // the sockets) is what makes that safe.

   for (size_t i = 0; i < m_aSocket.size (); i++)
      delete m_aSocket[i].pListener;

   m_aEntry.clear ();
   m_aSocket.clear ();
   m_aEvent.clear ();
}

// ---------------------------------------------------------------------------
// Entry_Find - index of a handle, or m_aEntry.size() when there is none. The
// store-qualified overload is what a guest Call uses: a handle is only valid to
// the store that opened it, so one guest can never name another's request.
// ---------------------------------------------------------------------------

size_t WASM_NETWORK::Entry_Find (WASM_STORE* pStore, uint64_t twRequestIx) const
{
   size_t nResult = m_aEntry.size ();

   for (size_t i = 0; i < m_aEntry.size ()  &&  nResult == m_aEntry.size (); i++)
   {
      if (m_aEntry[i].pStore == pStore  &&  m_aEntry[i].twRequestIx == twRequestIx)
         nResult = i;
   }

   return nResult;
}

size_t WASM_NETWORK::Entry_Find (uint64_t twRequestIx) const
{
   size_t nResult = m_aEntry.size ();

   for (size_t i = 0; i < m_aEntry.size ()  &&  nResult == m_aEntry.size (); i++)
   {
      if (m_aEntry[i].twRequestIx == twRequestIx)
         nResult = i;
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// Entry_Drop - remove one entry. Who closes its FILE depends on whether the
// fetch has landed: if the listener is still live the fetch is in flight, so
// the listener is orphaned and will close on arrival; otherwise the FILE is
// handed to the caller to close outside the lock.
// ---------------------------------------------------------------------------

void WASM_NETWORK::Entry_Drop (size_t nEntry, std::vector<SNEEZE::FILE*>& apClose)
{
   ENTRY& Entry = m_aEntry[nEntry];

   if (Entry.pListener)
      Entry.pListener->Orphan ();
   else if (Entry.pFile)
      apClose.push_back (Entry.pFile);

   m_aEntry.erase (m_aEntry.begin () + nEntry);
}

// ---------------------------------------------------------------------------
// Request_Open - reserve a handle and hold the caller's intent. Nothing is
// fetched yet: headers and the timeout are set between here and Send, exactly
// as XHR opens before it sends.
// ---------------------------------------------------------------------------

uint64_t WASM_NETWORK::Request_Open (WASM_STORE* pStore, uint64_t twFabricIx, eREQUEST_VERB eVerb, const std::string& sUrl, const std::string& sHash)
{
   uint64_t twRequestIx = 0;

   if (pStore  &&  !sUrl.empty ())
   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      ENTRY Entry;
      Entry.twRequestIx        = m_twRequest_Next++;
      Entry.pStore             = pStore;
      Entry.twFabricIx         = twFabricIx;
      Entry.pListener          = nullptr;
      Entry.pFile              = nullptr;
      Entry.Request.eVerb      = eVerb;
      Entry.Request.nSizeMax   = kREQUEST_SIZE_MAX;
      Entry.Request.bAnyStatus = true;
      Entry.sUrl               = sUrl;
      Entry.sHash              = sHash;
      Entry.eState             = kSNEEZE_ABI_REQUEST_STATE_IDLE;
      Entry.bAbort             = false;
      Entry.nHttpStatus        = 0;
      Entry.nSizeBytes         = 0;
      Entry.bCached            = false;

      m_aEntry.push_back (Entry);

      twRequestIx = Entry.twRequestIx;
   }

   return twRequestIx;
}

// ---------------------------------------------------------------------------
// Request_Header_Set - add or replace one request header. Only meaningful
// before Send; after that the request is on the wire and the answer is no.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Header_Set (WASM_STORE* pStore, uint64_t twRequestIx, const std::string& sName, const std::string& sValue)
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (pStore, twRequestIx);

   if (nEntry < m_aEntry.size ()  &&  m_aEntry[nEntry].eState == kSNEEZE_ABI_REQUEST_STATE_IDLE  &&  !sName.empty ())
   {
      m_aEntry[nEntry].Request.umsHeader[sName] = sValue;

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Request_Timeout_Set - per-request transport timeout, in milliseconds. Zero
// restores the engine default. Before Send only, same as the headers.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Timeout_Set (WASM_STORE* pStore, uint64_t twRequestIx, int32_t nMilli)
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (pStore, twRequestIx);

   if (nEntry < m_aEntry.size ()  &&  m_aEntry[nEntry].eState == kSNEEZE_ABI_REQUEST_STATE_IDLE  &&  nMilli >= 0)
   {
      m_aEntry[nEntry].Request.nTimeout_Milli = nMilli;

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Request_Send - put the request on the wire.
//
// Three phases, because File_Open reaches into CACHE and ASSET and m_mxNetwork
// is a leaf: snapshot the intent under the lock, open the file outside it, then
// re-take the lock to record the FILE. The fetch can land inside that window,
// which is exactly why the second phase only fills in what is still empty and
// never touches the state the completion may already have written.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Send (WASM_STORE* pStore, uint64_t twRequestIx, CACHE* pCache, const uint8_t* pBody, size_t nBody)
{
   bool bResult = false;

   REQUEST     Request;
   std::string sUrl;
   std::string sHash;
   LISTENER*   pListener = nullptr;

   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      size_t nEntry = Entry_Find (pStore, twRequestIx);

      if (pCache  &&  nEntry < m_aEntry.size ()  &&  m_aEntry[nEntry].eState == kSNEEZE_ABI_REQUEST_STATE_IDLE)
      {
         ENTRY& Entry = m_aEntry[nEntry];

         if (pBody  &&  nBody > 0)
            Entry.Request.aBody.assign (pBody, pBody + nBody);

         Entry.eState    = kSNEEZE_ABI_REQUEST_STATE_SENDING;
         Entry.pListener = new LISTENER (this, twRequestIx);

         Request   = Entry.Request;
         sUrl      = Entry.sUrl;
         sHash     = Entry.sHash;
         pListener = Entry.pListener;

         bResult = true;
      }
   }

   if (bResult)
   {
      SNEEZE::FILE* pFile = pCache->File_Open (sUrl, sHash, Request, pListener);

      std::lock_guard<std::mutex> guard (m_mxNetwork);

      size_t nEntry = Entry_Find (pStore, twRequestIx);

      if (nEntry < m_aEntry.size ()  &&  !m_aEntry[nEntry].pFile)
         m_aEntry[nEntry].pFile = pFile;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Request_Abort - stop reporting this request to its guest. The fetch itself
// keeps running (the transport has no cancel), but no event is delivered and
// the state reads ABORTED from here on. The handle stays valid until Close, so
// a guest that polls sees why nothing arrived.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Abort (WASM_STORE* pStore, uint64_t twRequestIx)
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (pStore, twRequestIx);

   if (nEntry < m_aEntry.size ())
   {
      ENTRY& Entry = m_aEntry[nEntry];

      Entry.bAbort = true;
      Entry.eState = kSNEEZE_ABI_REQUEST_STATE_ABORTED;

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Request_Close - the mirror of Open. Retires the handle and releases the FILE
// back to its cache, which keeps it for the inspector.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Close (WASM_STORE* pStore, uint64_t twRequestIx)
{
   bool bResult = false;

   std::vector<SNEEZE::FILE*> apClose;

   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      size_t nEntry = Entry_Find (pStore, twRequestIx);

      if (nEntry < m_aEntry.size ())
      {
         Entry_Drop (nEntry, apClose);

         bResult = true;
      }
   }

   for (auto* pFile : apClose)
      pFile->Close ();

   return bResult;
}

// ---------------------------------------------------------------------------
// Reading a completed request. Every one of these is a plain table read - the
// answer was snapshot when the fetch landed, so nothing here touches a FILE.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Result (WASM_STORE* pStore, uint64_t twRequestIx, RESULT& Result) const
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (pStore, twRequestIx);

   if (nEntry < m_aEntry.size ())
   {
      const ENTRY& Entry = m_aEntry[nEntry];

      Result.eState       = Entry.eState;
      Result.nHttpStatus  = Entry.nHttpStatus;
      Result.nSizeBytes   = Entry.nSizeBytes;
      Result.bCached      = Entry.bCached;
      Result.sUrl         = Entry.sUrl_Final.empty () ? Entry.sUrl : Entry.sUrl_Final;
      Result.sContentType = Entry.sContentType;
      Result.sError       = Entry.sError;

      bResult = true;
   }

   return bResult;
}

bool WASM_NETWORK::Request_Body (WASM_STORE* pStore, uint64_t twRequestIx, std::vector<uint8_t>& aBody) const
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (pStore, twRequestIx);

   if (nEntry < m_aEntry.size ())
   {
      aBody = m_aEntry[nEntry].aBody;

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Request_Header - one response header by name. Header names are matched
// case-insensitively, as HTTP defines them; the fetch machinery already stores
// them lowercased, so lowering the query is enough.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Header (WASM_STORE* pStore, uint64_t twRequestIx, const std::string& sName, std::string& sValue) const
{
   bool bResult = false;

   std::string sKey = sName;
   for (auto& c : sKey)
      c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (pStore, twRequestIx);

   if (nEntry < m_aEntry.size ())
   {
      auto it = m_aEntry[nEntry].umsRspHeader.find (sKey);

      if (it != m_aEntry[nEntry].umsRspHeader.end ())
         sValue = it->second;

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Request_Headers - every response header, in the CRLF-separated
// "name: value" form XHR's getAllResponseHeaders returns.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Headers (WASM_STORE* pStore, uint64_t twRequestIx, std::string& sHeaders) const
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (pStore, twRequestIx);

   if (nEntry < m_aEntry.size ())
   {
      for (const auto& pair : m_aEntry[nEntry].umsRspHeader)
         sHeaders += pair.first + ": " + pair.second + "\r\n";

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Request_Complete - the fetch has landed. Runs on a FETCH agent with the asset
// lock held, so the FILE is read FIRST (that is the only place the response is
// guaranteed to be on disk and intact) and the lock is taken afterwards, in the
// asset-then-network order the leaf rule requires.
//
// An aborted request records its outcome but delivers nothing: the guest said
// it had stopped listening.
//
// Returns false when the handle no longer exists, which tells the listener the
// FILE is now its responsibility to close.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Request_Complete (uint64_t twRequestIx, SNEEZE::FILE* pFile, bool bSuccess)
{
   bool bResult = false;

   std::vector<uint8_t> aBody;

   if (bSuccess)
      pFile->ReadData (aBody);

   long        nHttpStatus  = pFile->HttpStatus ();
   uint64_t    nSizeBytes   = pFile->SizeBytes ();
   bool        bCached      = pFile->IsServedFromCache ();
   std::string sUrl         = pFile->Url ();
   std::string sContentType = pFile->ContentType ();
   std::string sError       = pFile->Error ();

   std::unordered_map<std::string, std::string> umsRspHeader = pFile->RspHeaders ();

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nEntry = Entry_Find (twRequestIx);

   if (nEntry < m_aEntry.size ())
   {
      ENTRY& Entry = m_aEntry[nEntry];

      Entry.pListener    = nullptr;   // the listener retires itself the moment this returns
      Entry.pFile        = pFile;
      Entry.nHttpStatus  = nHttpStatus;
      Entry.nSizeBytes   = nSizeBytes;
      Entry.bCached      = bCached;
      Entry.sUrl_Final   = sUrl;
      Entry.sContentType = sContentType;
      Entry.sError       = sError;
      Entry.umsRspHeader = umsRspHeader;
      Entry.aBody        = aBody;

      if (!Entry.bAbort)
      {
         Entry.eState = bSuccess ? kSNEEZE_ABI_REQUEST_STATE_COMPLETE : kSNEEZE_ABI_REQUEST_STATE_FAILED;

         EVENT event;
         event.wMethod    = kSNEEZE_ABI_METHOD_NETWORK_REQUEST_COMPLETED;
         event.pStore     = Entry.pStore;
         event.twFabricIx = Entry.twFabricIx;
         event.twHandle   = Entry.twRequestIx;
         event.qwA        = bSuccess ? 1 : 0;
         event.qwB        = 0;

         m_aEvent.push_back (event);
      }

      bResult = true;
   }

   return bResult;
}

// ===========================================================================
// Sockets
// ===========================================================================

// ---------------------------------------------------------------------------
// Socket_Find / Socket_Drop / Socket_Retire - the socket mirrors of the request
// helpers above, with the same rules: Find returns m_aSocket.size() for "no such
// handle", Drop runs under m_mxNetwork and hands what it removed to the caller,
// and Retire does the part that must not happen under the lock.
// ---------------------------------------------------------------------------

size_t WASM_NETWORK::Socket_Find (WASM_STORE* pStore, uint64_t twSocketIx) const
{
   size_t nResult = m_aSocket.size ();

   for (size_t i = 0; i < m_aSocket.size ()  &&  nResult == m_aSocket.size (); i++)
   {
      if (m_aSocket[i].pStore == pStore  &&  m_aSocket[i].twSocketIx == twSocketIx)
         nResult = i;
   }

   return nResult;
}

size_t WASM_NETWORK::Socket_Find (uint64_t twSocketIx) const
{
   size_t nResult = m_aSocket.size ();

   for (size_t i = 0; i < m_aSocket.size ()  &&  nResult == m_aSocket.size (); i++)
   {
      if (m_aSocket[i].twSocketIx == twSocketIx)
         nResult = i;
   }

   return nResult;
}

void WASM_NETWORK::Socket_Drop (size_t nSocket, std::vector<SOCKET_ENTRY>& aRetire)
{
   aRetire.push_back (m_aSocket[nSocket]);

   m_aSocket.erase (m_aSocket.begin () + nSocket);
}

void WASM_NETWORK::Socket_Retire (std::vector<SOCKET_ENTRY>& aRetire)
{
   for (auto& Entry : aRetire)
   {
      // Order matters and is the whole reason this is not done under the lock:
      // NETWORK::Socket_Close promises no callback is running or will run once
      // it returns, which is what makes deleting the listener next safe.
      if (Entry.pSocket)
         m_pEngine->Network ()->Socket_Close (Entry.pSocket);

      delete Entry.pListener;
   }

   aRetire.clear ();
}

// ---------------------------------------------------------------------------
// Socket_Open - reserve a handle and start connecting.
//
// The entry is listed before the socket exists, because a handshake can fail
// before NETWORK::Socket_Open even returns and the callback reporting it has to
// find something to report against.
// ---------------------------------------------------------------------------

uint64_t WASM_NETWORK::Socket_Open (WASM_STORE* pStore, uint64_t twFabricIx, CONTAINER* pContainer, const std::string& sUrl, const std::string& sProtocol)
{
   uint64_t twSocketIx = 0;

   if (pStore  &&  pContainer  &&  !sUrl.empty ())
   {
      SOCKET_LISTENER* pListener = nullptr;

      {
         std::lock_guard<std::mutex> guard (m_mxNetwork);

         SOCKET_ENTRY Entry;
         Entry.twSocketIx = m_twSocket_Next++;
         Entry.pStore     = pStore;
         Entry.twFabricIx = twFabricIx;
         Entry.pSocket    = nullptr;
         Entry.pListener  = new SOCKET_LISTENER (this, Entry.twSocketIx);
         Entry.eState     = kSNEEZE_ABI_SOCKET_STATE_CONNECTING;
         Entry.sUrl       = sUrl;
         Entry.nQueued    = 0;

         m_aSocket.push_back (Entry);

         twSocketIx = Entry.twSocketIx;
         pListener  = Entry.pListener;
      }

      // Outside the lock: this reaches into NETWORK and the socket hub, and
      // m_mxNetwork is a leaf.
      SNEEZE::SOCKET* pSocket = m_pEngine->Network ()->Socket_Open (pContainer, sUrl, sProtocol, pListener);

      std::vector<SOCKET_ENTRY> aRetire;

      {
         std::lock_guard<std::mutex> guard (m_mxNetwork);

         size_t nSocket = Socket_Find (twSocketIx);

         if (nSocket < m_aSocket.size ())
         {
            if (pSocket)
            {
               m_aSocket[nSocket].pSocket = pSocket;
            }
            else
            {
               // Not a WebSocket URL. The handle is withdrawn rather than handed
               // back dead, so the guest gets the 0 the ABI defines for this.
               Socket_Drop (nSocket, aRetire);

               twSocketIx = 0;
            }
         }
      }

      Socket_Retire (aRetire);
   }

   return twSocketIx;
}

// ---------------------------------------------------------------------------
// Socket_Send - one frame, text or binary.
//
// The socket is read out under the lock and used outside it. Nothing can free it
// in that window: only the store that opened a handle can name it, and a guest
// Call already holds that store's lock.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Socket_Send (WASM_STORE* pStore, uint64_t twSocketIx, const uint8_t* pData, size_t nSize, bool bBinary)
{
   bool bResult = false;

   SNEEZE::SOCKET* pSocket = nullptr;

   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      size_t nSocket = Socket_Find (pStore, twSocketIx);

      if (nSocket < m_aSocket.size ())
         pSocket = m_aSocket[nSocket].pSocket;
   }

   if (pSocket)
   {
      if (bBinary)
         bResult = pSocket->Send_Binary (pData, nSize);
      else
         bResult = pSocket->Send_Text (std::string (reinterpret_cast<const char*> (pData), nSize));
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Socket_Close - the closing handshake. The handle stays valid and readable
// afterwards, exactly as the browser's does; Socket_Free is what retires it.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Socket_Close (WASM_STORE* pStore, uint64_t twSocketIx, uint16_t wCode, const std::string& sReason)
{
   bool bResult = false;

   SNEEZE::SOCKET* pSocket = nullptr;

   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      size_t nSocket = Socket_Find (pStore, twSocketIx);

      if (nSocket < m_aSocket.size ())
      {
         pSocket = m_aSocket[nSocket].pSocket;

         if (m_aSocket[nSocket].eState == kSNEEZE_ABI_SOCKET_STATE_CONNECTING  ||  m_aSocket[nSocket].eState == kSNEEZE_ABI_SOCKET_STATE_OPEN)
            m_aSocket[nSocket].eState = kSNEEZE_ABI_SOCKET_STATE_CLOSING;

         bResult = true;
      }
   }

   if (pSocket)
      pSocket->Close (wCode, sReason);

   return bResult;
}

// ---------------------------------------------------------------------------
// Socket_Free - the mirror of Open. Retires the handle and takes the connection
// with it.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Socket_Free (WASM_STORE* pStore, uint64_t twSocketIx)
{
   bool bResult = false;

   std::vector<SOCKET_ENTRY> aRetire;

   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      size_t nSocket = Socket_Find (pStore, twSocketIx);

      if (nSocket < m_aSocket.size ())
      {
         Socket_Drop (nSocket, aRetire);

         bResult = true;
      }
   }

   Socket_Retire (aRetire);

   return bResult;
}

// ---------------------------------------------------------------------------
// Reading a socket. State, the URL, the negotiated subprotocol and the error are
// all kept current in the table by the callbacks, so this is a plain table read
// like a completed request's - only Buffered has to ask the socket itself,
// because it changes with every byte that leaves.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Socket_Result (WASM_STORE* pStore, uint64_t twSocketIx, SOCKET_RESULT& Result) const
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nSocket = Socket_Find (pStore, twSocketIx);

   if (nSocket < m_aSocket.size ())
   {
      const SOCKET_ENTRY& Entry = m_aSocket[nSocket];

      Result.eState    = Entry.eState;
      Result.sUrl      = Entry.sUrl;
      Result.sProtocol = Entry.sProtocol;
      Result.sError    = Entry.sError;

      bResult = true;
   }

   return bResult;
}

uint64_t WASM_NETWORK::Socket_Buffered (WASM_STORE* pStore, uint64_t twSocketIx) const
{
   uint64_t nResult = 0;

   SNEEZE::SOCKET* pSocket = nullptr;

   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      size_t nSocket = Socket_Find (pStore, twSocketIx);

      if (nSocket < m_aSocket.size ())
         pSocket = m_aSocket[nSocket].pSocket;
   }

   if (pSocket)
      nResult = pSocket->Buffered ();

   return nResult;
}

// ---------------------------------------------------------------------------
// Socket_Recv - the head of the receive queue. One RECEIVED notify, one message:
// the guest is told a message arrived and how big it is, and this is where it
// takes it.
//
// The message is only removed once it has somewhere to go. A caller with no room
// (or with none yet, asking only for the size) gets the bytes reported but the
// queue left alone, so nothing is ever half-delivered and then lost.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Socket_Recv (WASM_STORE* pStore, uint64_t twSocketIx, size_t nCapacity, std::vector<uint8_t>& aData, bool& bBinary)
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nSocket = Socket_Find (pStore, twSocketIx);

   if (nSocket < m_aSocket.size ()  &&  !m_aSocket[nSocket].aMessage.empty ())
   {
      SOCKET_ENTRY& Entry = m_aSocket[nSocket];

      aData   = Entry.aMessage.front ().aData;
      bBinary = Entry.aMessage.front ().bBinary;

      if (nCapacity >= aData.size ())
      {
         Entry.nQueued -= (aData.size () < Entry.nQueued) ? aData.size () : Entry.nQueued;

         Entry.aMessage.erase (Entry.aMessage.begin ());
      }

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// The four callbacks, from the network's io thread. Each records what changed
// and queues the guest's event; none of them touch a SOCKET, so m_mxNetwork
// stays a leaf in this direction too.
// ---------------------------------------------------------------------------

void WASM_NETWORK::Socket_Opened (uint64_t twSocketIx, const std::string& sProtocol)
{
   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nSocket = Socket_Find (twSocketIx);

   if (nSocket < m_aSocket.size ())
   {
      SOCKET_ENTRY& Entry = m_aSocket[nSocket];

      Entry.eState    = kSNEEZE_ABI_SOCKET_STATE_OPEN;
      Entry.sProtocol = sProtocol;

      EVENT event;
      event.wMethod    = kSNEEZE_ABI_METHOD_NETWORK_SOCKET_OPENED;
      event.pStore     = Entry.pStore;
      event.twFabricIx = Entry.twFabricIx;
      event.twHandle   = Entry.twSocketIx;
      event.qwA        = 0;
      event.qwB        = 0;

      m_aEvent.push_back (event);
   }
}

void WASM_NETWORK::Socket_Message (uint64_t twSocketIx, const uint8_t* pData, size_t nSize, bool bBinary)
{
   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nSocket = Socket_Find (twSocketIx);

   if (nSocket < m_aSocket.size ())
   {
      SOCKET_ENTRY& Entry = m_aSocket[nSocket];

      if (Entry.nQueued + nSize <= SOCKET_QUEUE_MAX)
      {
         MESSAGE Message;
         Message.aData.assign (pData, pData + nSize);
         Message.bBinary = bBinary;

         Entry.aMessage.push_back (Message);
         Entry.nQueued += nSize;

         EVENT event;
         event.wMethod    = kSNEEZE_ABI_METHOD_NETWORK_SOCKET_RECEIVED;
         event.pStore     = Entry.pStore;
         event.twFabricIx = Entry.twFabricIx;
         event.twHandle   = Entry.twSocketIx;
         event.qwA        = bBinary ? 1 : 0;
         event.qwB        = nSize;

         m_aEvent.push_back (event);
      }
      else
      {
         // The guest has stopped draining. The socket keeps running and the
         // message is dropped - it is not silent, because the error says so.
         Entry.sError = "the receive queue overflowed; messages were dropped";
      }
   }
}

void WASM_NETWORK::Socket_Failed (uint64_t twSocketIx, const std::string& sError)
{
   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nSocket = Socket_Find (twSocketIx);

   if (nSocket < m_aSocket.size ())
   {
      SOCKET_ENTRY& Entry = m_aSocket[nSocket];

      Entry.eState = kSNEEZE_ABI_SOCKET_STATE_CLOSED;
      Entry.sError = sError;

      EVENT event;
      event.wMethod    = kSNEEZE_ABI_METHOD_NETWORK_SOCKET_FAILED;
      event.pStore     = Entry.pStore;
      event.twFabricIx = Entry.twFabricIx;
      event.twHandle   = Entry.twSocketIx;
      event.qwA        = 0;
      event.qwB        = 0;

      m_aEvent.push_back (event);
   }
}

void WASM_NETWORK::Socket_Closed (uint64_t twSocketIx, uint16_t wCode, bool bClean)
{
   std::lock_guard<std::mutex> guard (m_mxNetwork);

   size_t nSocket = Socket_Find (twSocketIx);

   if (nSocket < m_aSocket.size ())
   {
      SOCKET_ENTRY& Entry = m_aSocket[nSocket];

      Entry.eState = kSNEEZE_ABI_SOCKET_STATE_CLOSED;

      EVENT event;
      event.wMethod    = kSNEEZE_ABI_METHOD_NETWORK_SOCKET_CLOSED;
      event.pStore     = Entry.pStore;
      event.twFabricIx = Entry.twFabricIx;
      event.twHandle   = Entry.twSocketIx;
      event.qwA        = wCode;
      event.qwB        = bClean ? 1 : 0;

      m_aEvent.push_back (event);
   }
}

// ---------------------------------------------------------------------------
// Fabric_Close - a fabric is going away, so its requests go with it. Handles it
// opened are no longer nameable by anyone.
// ---------------------------------------------------------------------------

void WASM_NETWORK::Fabric_Close (WASM_STORE* pStore, uint64_t twFabricIx)
{
   std::vector<SNEEZE::FILE*> apClose;
   std::vector<SOCKET_ENTRY>  aRetire;

   {
      std::lock_guard<std::mutex> guard (m_mxNetwork);

      for (size_t i = 0; i < m_aEntry.size (); )
      {
         if (m_aEntry[i].pStore == pStore  &&  m_aEntry[i].twFabricIx == twFabricIx)
            Entry_Drop (i, apClose);
         else
            i++;
      }

      for (size_t i = 0; i < m_aSocket.size (); )
      {
         if (m_aSocket[i].pStore == pStore  &&  m_aSocket[i].twFabricIx == twFabricIx)
            Socket_Drop (i, aRetire);
         else
            i++;
      }

      for (size_t i = 0; i < m_aEvent.size (); )
      {
         if (m_aEvent[i].pStore == pStore  &&  m_aEvent[i].twFabricIx == twFabricIx)
            m_aEvent.erase (m_aEvent.begin () + i);
         else
            i++;
      }
   }

   for (auto* pFile : apClose)
      pFile->Close ();

   Socket_Retire (aRetire);
}

// ---------------------------------------------------------------------------
// Store_Close - drop every request in a store being torn down, discard its
// undelivered events, and block until no delivery is still in flight for it.
// After this returns no network agent holds or will claim anything for pStore,
// so the store can be deleted.
//
// This runs before the container closes its CACHE, which is what makes the
// closes below safe: the FILEs are still alive and still owned by that cache.
// ---------------------------------------------------------------------------

void WASM_NETWORK::Store_Close (WASM_STORE* pStore)
{
   std::vector<SNEEZE::FILE*> apClose;
   std::vector<SOCKET_ENTRY>  aRetire;

   {
      std::unique_lock<std::mutex> lock (m_mxNetwork);

      for (size_t i = 0; i < m_aEntry.size (); )
      {
         if (m_aEntry[i].pStore == pStore)
            Entry_Drop (i, apClose);
         else
            i++;
      }

      for (size_t i = 0; i < m_aSocket.size (); )
      {
         if (m_aSocket[i].pStore == pStore)
            Socket_Drop (i, aRetire);
         else
            i++;
      }

      for (size_t i = 0; i < m_aEvent.size (); )
      {
         if (m_aEvent[i].pStore == pStore)
            m_aEvent.erase (m_aEvent.begin () + i);
         else
            i++;
      }

      while (m_nInFlight > 0)
         m_cvNetwork.wait (lock);
   }

   for (auto* pFile : apClose)
      pFile->Close ();

   // No event can reappear for this store while the above runs: dropping the
   // entries is what stops it, because a callback that finds no entry queues
   // nothing - the same protection orphaning a request listener provides.
   Socket_Retire (aRetire);
}

// ---------------------------------------------------------------------------
// Claim - hand the oldest queued event to an agent. Events are delivered in the
// order they were produced, so a guest sees its completions in the order the
// network answered.
//
// The in-flight count is deliberately whole-service rather than per-store: it
// exists only so Store_Close can wait out a delivery that might name its store,
// and the queue is short-lived enough that the coarser wait costs nothing.
// ---------------------------------------------------------------------------

bool WASM_NETWORK::Claim (EVENT& event)
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   if (!m_aEvent.empty ())
   {
      event = m_aEvent.front ();

      m_aEvent.erase (m_aEvent.begin ());

      m_nInFlight++;

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Complete - the delivery is done. Wakes Store_Close, which may be waiting it
// out.
// ---------------------------------------------------------------------------

void WASM_NETWORK::Complete (const EVENT& event)
{
   (void) event;

   std::lock_guard<std::mutex> guard (m_mxNetwork);

   m_nInFlight--;

   m_cvNetwork.notify_all ();
}
