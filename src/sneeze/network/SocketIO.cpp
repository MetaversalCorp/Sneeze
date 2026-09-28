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

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <sio_client.h>
#include <nlohmann/json.hpp>

using namespace SNEEZE;

static bool SocketIO_Url_Ok (const std::string& sUrl)
{
   bool bResult = false;

   if (sUrl.compare (0, 8, "https://") == 0
    ||  sUrl.compare (0, 7, "http://")  == 0
    ||  sUrl.compare (0, 6, "wss://")   == 0
    ||  sUrl.compare (0, 5, "ws://")    == 0)
   {
      bResult = true;
   }

   return bResult;
}

static bool SocketIO_Event_Builtin (const std::string& sEvent)
{
   bool bResult = false;

   if (sEvent == "connect"  ||  sEvent == "disconnect"  ||  sEvent == "connect_error")
      bResult = true;

   return bResult;
}

static nlohmann::json Json_From_Message (sio::message::ptr const& pMsg)
{
   nlohmann::json jResult = nullptr;

   if (pMsg)
   {
      switch (pMsg->get_flag ())
      {
         case sio::message::flag_integer: jResult = pMsg->get_int ();    break;
         case sio::message::flag_double:  jResult = pMsg->get_double (); break;
         case sio::message::flag_string:  jResult = pMsg->get_string (); break;
         case sio::message::flag_boolean: jResult = pMsg->get_bool ();   break;
         case sio::message::flag_null:    jResult = nullptr;             break;
         case sio::message::flag_binary:  jResult = nullptr;             break;
         case sio::message::flag_array:
         {
            jResult = nlohmann::json::array ();

            for (auto const& pItem : pMsg->get_vector ())
               jResult.push_back (Json_From_Message (pItem));
         } break;

         case sio::message::flag_object:
         {
            jResult = nlohmann::json::object ();

            for (auto const& kv : pMsg->get_map ())
               jResult[kv.first] = Json_From_Message (kv.second);
         } break;
      }
   }

   return jResult;
}

// One incoming Socket.IO argument list becomes one guest payload. A lone
// string stays raw text (not JSON-quoted). A lone binary stays bytes. Anything
// else is stringified as JSON, which is the host contract for objects.

static void Message_Pack (sio::message::list const& List, std::vector<uint8_t>& aData, bool& bBinary)
{
   aData.clear ();
   bBinary = false;

   if (List.size () == 1  &&  List[0]  &&  List[0]->get_flag () == sio::message::flag_binary)
   {
      std::shared_ptr<const std::string> pBinary = List[0]->get_binary ();

      if (pBinary)
         aData.assign (pBinary->begin (), pBinary->end ());

      bBinary = true;
   }
   else if (List.size () == 1  &&  List[0]  &&  List[0]->get_flag () == sio::message::flag_string)
   {
      std::string const& sText = List[0]->get_string ();

      aData.assign (sText.begin (), sText.end ());
   }
   else if (List.size () == 1)
   {
      std::string sText = Json_From_Message (List[0]).dump ();

      aData.assign (sText.begin (), sText.end ());
   }
   else if (List.size () > 1)
   {
      nlohmann::json jArray = nlohmann::json::array ();

      for (size_t i = 0; i < List.size (); i++)
         jArray.push_back (Json_From_Message (List[i]));

      std::string sText = jArray.dump ();

      aData.assign (sText.begin (), sText.end ());
   }
}

/***********************************************************************************************************************************
**  SOCKETIO Impl Class
***********************************************************************************************************************************/

class SOCKETIO::Impl
{
public:
   Impl (SOCKETIO* pSocketIO, CONTAINER* pContainer, uint32_t nSocketIOIx, const std::string& sUrl) :
      m_pSocketIO    (pSocketIO),
      m_pContainer   (pContainer),
      m_nSocketIOIx  (nSocketIOIx),
      m_sUrl         (sUrl),
      m_pClient      (nullptr),
      m_eState       (kSOCKETIO_STATE_CLOSED),
      m_pListener    (nullptr),
      m_bDetached    (false),
      m_bClose_Guest (false)
   {
   }

   ~Impl ()
   {
      {
         std::lock_guard<std::mutex> guard (m_mxSocketIO);

         m_bDetached = true;
         m_pListener = nullptr;
      }

      // Listeners off first, then sync_close, so a callback cannot run after
      // this returns. The lock is not held across sync_close: a late callback
      // that already started will take it, see bDetached, and leave.
      if (m_pClient)
      {
         m_pClient->socket ()->off_all ();
         m_pClient->clear_con_listeners ();
         m_pClient->sync_close ();

         delete m_pClient;

         m_pClient = nullptr;
      }
   }

   bool Initialize (ISOCKETIO* pListener)
   {
      bool bResult = false;

      if (SocketIO_Url_Ok (m_sUrl))
      {
         {
            std::lock_guard<std::mutex> guard (m_mxSocketIO);

            m_pListener = pListener;
            m_eState    = kSOCKETIO_STATE_CONNECTING;
         }

         m_pClient = new sio::client ();

         m_pClient->set_logs_quiet ();
         m_pClient->set_reconnect_attempts (0);

         m_pClient->set_open_listener  ([this] () { OnOpened (); });
         m_pClient->set_fail_listener  ([this] () { OnFailed (); });
         m_pClient->set_close_listener ([this] (sio::client::close_reason const&) { OnClosed (); });

         // Registered before connect so the first custom event is not missed.
         // connect / disconnect / connect_error stay off this path.
         m_pClient->socket ()->on_any ([this] (sio::event& Event)
         {
            OnEvent (Event);
         });

         m_pClient->connect (m_sUrl);

         bResult = true;
      }
      else
      {
         std::lock_guard<std::mutex> guard (m_mxSocketIO);

         m_eState = kSOCKETIO_STATE_CLOSED;
         m_sError = "not a Socket.IO URL, or the connection could not be created";
      }

      return bResult;
   }

   bool Emit (const std::string& sEvent, const uint8_t* pData, size_t nSize, bool bBinary, bool bAck, uint64_t qwParam)
   {
      bool bResult = false;

      if (!sEvent.empty ()  &&  nSize <= kSOCKETIO_PAYLOAD_MAX  &&  State () == kSOCKETIO_STATE_OPEN  &&  m_pClient)
      {
         sio::message::list List;

         std::string sPayload;

         if (pData  &&  nSize > 0)
            sPayload.assign (reinterpret_cast<const char*> (pData), nSize);

         if (bBinary)
         {
            std::shared_ptr<const std::string> pBinary = std::make_shared<const std::string> (std::move (sPayload));

            List.push (pBinary);
         }
         else
         {
            List.push (sPayload);
         }

         if (bAck)
         {
            m_pClient->socket ()->emit (sEvent, List, [this, qwParam] (sio::message::list const& Ack)
            {
               OnAck (qwParam, Ack);
            });
         }
         else
         {
            m_pClient->socket ()->emit (sEvent, List);
         }

         bResult = true;
      }

      return bResult;
   }

   void Close ()
   {
      bool bClose = false;

      {
         std::lock_guard<std::mutex> guard (m_mxSocketIO);

         if (m_eState == kSOCKETIO_STATE_CONNECTING  ||  m_eState == kSOCKETIO_STATE_OPEN)
         {
            m_eState       = kSOCKETIO_STATE_CLOSING;
            m_bClose_Guest = true;
            bClose         = true;
         }
      }

      if (bClose  &&  m_pClient)
         m_pClient->close ();
   }

   eSOCKETIO_STATE State () const
   {
      std::lock_guard<std::mutex> guard (m_mxSocketIO);

      return m_eState;
   }

   std::string Error () const
   {
      std::lock_guard<std::mutex> guard (m_mxSocketIO);

      return m_sError;
   }

   ISOCKETIO* Listener () const
   {
      std::lock_guard<std::mutex> guard (m_mxSocketIO);

      return m_pListener;
   }

   void OnOpened ()
   {
      ISOCKETIO* pListener = nullptr;

      {
         std::lock_guard<std::mutex> guard (m_mxSocketIO);

         if (!m_bDetached)
         {
            m_eState  = kSOCKETIO_STATE_OPEN;
            pListener = m_pListener;
         }
      }

      if (pListener)
         pListener->OnSocketIOOpened (m_pSocketIO);
   }

   void OnEvent (sio::event& Event)
   {
      std::string sEvent = Event.get_name ();

      if (!SocketIO_Event_Builtin (sEvent))
      {
         std::vector<uint8_t> aData;
         bool                 bBinary = false;

         Message_Pack (Event.get_messages (), aData, bBinary);

         if (aData.size () > kSOCKETIO_PAYLOAD_MAX)
         {
            std::lock_guard<std::mutex> guard (m_mxSocketIO);

            m_sError = "a payload exceeded the 16 MB cap";
         }
         else
         {
            ISOCKETIO* pListener = nullptr;

            {
               std::lock_guard<std::mutex> guard (m_mxSocketIO);

               if (!m_bDetached)
                  pListener = m_pListener;
            }

            if (pListener)
               pListener->OnSocketIOEvent (m_pSocketIO, sEvent, aData.data (), aData.size (), bBinary);
         }
      }
   }

   void OnAck (uint64_t qwParam, sio::message::list const& Ack)
   {
      std::vector<uint8_t> aData;
      bool                 bBinary = false;

      Message_Pack (Ack, aData, bBinary);

      if (aData.size () > kSOCKETIO_PAYLOAD_MAX)
      {
         std::lock_guard<std::mutex> guard (m_mxSocketIO);

         m_sError = "a payload exceeded the 16 MB cap";
      }
      else
      {
         ISOCKETIO* pListener = nullptr;

         {
            std::lock_guard<std::mutex> guard (m_mxSocketIO);

            if (!m_bDetached)
               pListener = m_pListener;
         }

         if (pListener)
            pListener->OnSocketIOAck (m_pSocketIO, qwParam, aData.data (), aData.size (), bBinary);
      }
   }

   void OnFailed ()
   {
      ISOCKETIO* pListener = nullptr;

      {
         std::lock_guard<std::mutex> guard (m_mxSocketIO);

         if (!m_bDetached  &&  m_eState != kSOCKETIO_STATE_CLOSED)
         {
            m_eState = kSOCKETIO_STATE_CLOSED;

            if (m_sError.empty ())
               m_sError = "the Socket.IO handshake failed";

            pListener = m_pListener;
         }
      }

      // Same shape as SOCKET: the error is reported, then a 1006 close,
      // because no clean disconnect happened.
      if (pListener)
      {
         pListener->OnSocketIOFailed (m_pSocketIO);
         pListener->OnSocketIOClosed (m_pSocketIO, 1006, false);
      }
   }

   void OnClosed ()
   {
      ISOCKETIO* pListener = nullptr;
      bool       bClean    = false;

      {
         std::lock_guard<std::mutex> guard (m_mxSocketIO);

         if (!m_bDetached  &&  m_eState != kSOCKETIO_STATE_CLOSED)
         {
            m_eState  = kSOCKETIO_STATE_CLOSED;
            bClean    = m_bClose_Guest;
            pListener = m_pListener;
         }
      }

      if (pListener)
         pListener->OnSocketIOClosed (m_pSocketIO, bClean ? 1000 : 1006, bClean);
   }

   SOCKETIO*                                          m_pSocketIO;
   CONTAINER*                                         m_pContainer;
   uint32_t                                           m_nSocketIOIx;
   std::string                                        m_sUrl;
   sio::client*                                       m_pClient;

   mutable std::mutex                                 m_mxSocketIO;
   eSOCKETIO_STATE                                    m_eState;
   std::string                                        m_sError;
   ISOCKETIO*                                         m_pListener;
   bool                                               m_bDetached;
   bool                                               m_bClose_Guest;
};

/***********************************************************************************************************************************
**  SOCKETIO
***********************************************************************************************************************************/

SOCKETIO::SOCKETIO (CONTAINER* pContainer, uint32_t nSocketIOIx, const std::string& sUrl) :
   m_pImpl (new Impl (this, pContainer, nSocketIOIx, sUrl))
{
}

SOCKETIO::~SOCKETIO ()
{
   delete m_pImpl;
}

bool SOCKETIO::Initialize (ISOCKETIO* pListener) { return m_pImpl->Initialize (pListener); }

bool SOCKETIO::Emit_Text (const std::string& sEvent, const std::string& sText)
{
   return m_pImpl->Emit (sEvent, reinterpret_cast<const uint8_t*> (sText.data ()), sText.size (), false, false, 0);
}

bool SOCKETIO::Emit_Binary (const std::string& sEvent, const uint8_t* pData, size_t nSize)
{
   return m_pImpl->Emit (sEvent, pData, nSize, true, false, 0);
}

bool SOCKETIO::Emit_Text_Ex (const std::string& sEvent, const std::string& sText, uint64_t qwParam)
{
   return m_pImpl->Emit (sEvent, reinterpret_cast<const uint8_t*> (sText.data ()), sText.size (), false, true, qwParam);
}

bool SOCKETIO::Emit_Binary_Ex (const std::string& sEvent, const uint8_t* pData, size_t nSize, uint64_t qwParam)
{
   return m_pImpl->Emit (sEvent, pData, nSize, true, true, qwParam);
}

void SOCKETIO::Close () { m_pImpl->Close (); }

eSOCKETIO_STATE      SOCKETIO::State    () const { return m_pImpl->State (); }
uint64_t             SOCKETIO::Buffered () const { return 0; }
const std::string&   SOCKETIO::Url      () const { return m_pImpl->m_sUrl; }
std::string          SOCKETIO::Error    () const { return m_pImpl->Error (); }

uint32_t             SOCKETIO::SocketIOIx () const { return m_pImpl->m_nSocketIOIx; }
CONTAINER*           SOCKETIO::Container  () const { return m_pImpl->m_pContainer; }
ISOCKETIO*           SOCKETIO::Listener   () const { return m_pImpl->Listener (); }

std::string          SOCKETIO::ContainerName () const { return m_pImpl->m_pContainer->Identity ()->DisplayName (); }
