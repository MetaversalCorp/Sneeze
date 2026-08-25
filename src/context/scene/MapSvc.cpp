/*******************************************************************************************************************************
**                                                                                                                            **
**                                      MVSAMPLE_cpp : MAPSVC.cpp                                                             **
**                                                                                                                            **
********************************************************************************************************************************
**                                                                                                                            **
*******************************************************************************************************************************/

#include "MapSvc.h"

using namespace SNEEZE;

// Maps a map-object class to the LnG model id used to Model_Open (subscribe) it.
// Children arrive from Child_Enum as PARTIAL stubs; opening the model against the
// LnG is what fetches the node's own children so the next tier can be enumerated.
static const char* MapModelId (uint16_t wClass)
{
   const char* sID = nullptr;

   switch (wClass)
   {
   case RMAP::MAP::MAP_OBJECT_CLASS_ROOT:         sID = "RMRoot";     break;
   case RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL:    sID = "RMCObject";  break;
   case RMAP::MAP::MAP_OBJECT_CLASS_TERRESTRIAL:  sID = "RMTObject";  break;
   case RMAP::MAP::MAP_OBJECT_CLASS_PHYSICAL:     sID = "RMPObject";  break;
   }

   return sID;
}

/*******************************************************************************************************************************
**                                                     CLASS (FABRIC)                                                         **
*******************************************************************************************************************************/

class MAPSVC::Impl : public RMAP::CORE::NOTIFICATION
{
public:
   enum eSTATE
   {
      NOTREADY                = 0,
      LOGGINGIN_AUTHENTICATE  = 1,
      READY_LOGGEDOUT         = 2,
      READY_LOGGEDIN          = 3
   };

public:
   Impl (CONTAINER* pContainer, uint64_t twFabricIx, const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map) :
      m_wClass_Map (wClass_Map),
      m_twObjectIx_Map (twObjectIx_Map),
      m_pContainer (pContainer),
      m_twFabricIx (twFabricIx),
      m_pLnG (nullptr)
   {
      RMAP::CORE::APP* pCore = RMAP::CORE::APP::GetInstance ();

      if ((m_pRequire = pCore->Require ("Map", sService, sNamespace)) != NULL)
      {
         if ((m_pLnG = pCore->LnG_Open (sNamespace, sService, sConnect, "")) != NULL)
            m_pLnG->Attach (this);
      }
   }

   ~Impl ()
   {
   }

   void onReadyState (RMAP::CORE::INOTICE* pNotice)
   {
      if (pNotice->pCreator == m_pLnG)
      {
         switch (m_pLnG->ReadyState ())
         {
         case RMAP::CORE::LNG::eSTATE::DISCONNECTED: // UI Update: Disconnected 
            ReadyState (MAPSVC::Impl::eSTATE::NOTREADY);
            break;

         case RMAP::CORE::LNG::eSTATE::LOGGEDOUT:  // UI Update: Not Logged In
            ReadyState (MAPSVC::Impl::eSTATE::READY_LOGGEDOUT);
            break;

         case RMAP::CORE::LNG::eSTATE::LOGGEDIN:
            ReadyState (MAPSVC::Impl::eSTATE::READY_LOGGEDIN);
            break;
         }
      }
   }

   void Notify (RMAP::CORE::INOTICE* pNotice) override
   {
      if (pNotice->sNotification.compare ("onReadyState") == 0)
         onReadyState (pNotice);
   }

   bool IsReady ()
   {
      int nReadyState = ReadyState ();

      return (nReadyState == eSTATE::READY_LOGGEDIN || nReadyState == eSTATE::READY_LOGGEDOUT);
   }

public:
   RMAP::CORE::LNG*                    m_pLnG;
   uint16_t                            m_wClass_Map;
   uint64_t                            m_twObjectIx_Map;

   CONTAINER*                          m_pContainer;
   uint64_t                            m_twFabricIx;

private:
   RMAP::CORE::APP::REQUIRE*           m_pRequire;
};

/*******************************************************************************************************************************
**                                                     CLASS (MAPSVC)                                                         **
*******************************************************************************************************************************/

MAPSVC::MAPSVC (CONTAINER* pContainer, uint64_t twFabricIx, const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map) :
   m_pImpl (new Impl (pContainer, twFabricIx, sNamespace, sService, sConnect, wClass_Map, twObjectIx_Map)),
   m_pRMXRoot (NULL)
{
   m_pImpl->Attach (this);
}

MAPSVC::~MAPSVC ()
{
   // Detach Expand subscriptions, then Model_Close every handle we Model_Open'd.
   // pRMXOpen is the Node_Open pairing (closed here if Unregister did not run).
   // pRMXSub is Expand's attach of that same handle, or a second Model_Open when
   // OpenChild had none -- close it only when it is not pRMXOpen (one Close).
   // Child_Enum stubs (pRMXObject) are owned by their parent's collection.
   // The root is closed explicitly below (its pRMXSub aliases m_pRMXRoot).
   for (auto& elem : m_mpRMObject)
   {
      ITEM& Item = elem.second;

      if (Item.pRMXSub  &&  Item.pRMXSub != m_pRMXRoot)
         Item.pRMXSub->Detach (this);

      if (Item.pRMXSub  &&  Item.pRMXSub != m_pRMXRoot  &&  Item.pRMXSub != Item.pRMXOpen)
         m_pImpl->m_pLnG->Model_Close (Item.pRMXSub);

      if (Item.pRMXOpen  &&  Item.pRMXOpen != m_pRMXRoot)
         m_pImpl->m_pLnG->Model_Close (Item.pRMXOpen);
   }

   if (m_pRMXRoot)
   {
      m_pRMXRoot->Detach (this);
      m_pImpl->m_pLnG->Model_Close (m_pRMXRoot);
   }

   delete m_pImpl;
}

void MAPSVC::Notify (RMAP::CORE::INOTICE* pNotice)
{
   if (pNotice->sNotification.compare ("onReadyState") == 0)
      onReadyState (pNotice);
}

void MAPSVC::ReadyStateEx (int nReadyState)
{
   std::string sID_Model;

   switch (m_pImpl->m_wClass_Map)
   {
   case RMAP::MAP::MAP_OBJECT_CLASS_ROOT:         sID_Model = "RMRoot";     break;
   case RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL:    sID_Model = "RMCObject";  break;
   case RMAP::MAP::MAP_OBJECT_CLASS_TERRESTRIAL:  sID_Model = "RMTObject";  break;
   case RMAP::MAP::MAP_OBJECT_CLASS_PHYSICAL:     sID_Model = "RMPObject";  break;
   }

   if (sID_Model.empty () == false)
   {
      char sHex[1024];
      sprintf (sHex, "Open Model ROOT: 0x%llX", m_pImpl->m_twObjectIx_Map);
      m_pImpl->m_pContainer->Context ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "MAPSVC", sHex);

      m_pRMXRoot = dynamic_cast <RMAP::CORE::MODEL_OBJECT*> (m_pImpl->m_pLnG->Model_Open (sID_Model, std::to_string (m_pImpl->m_twObjectIx_Map)));
      m_pRMXRoot->Attach (this, false, true);
   }
}

void MAPSVC::LoadChildren (RMAP::CORE::MODEL_OBJECT* pRMXSub)
{
   // Held across the enumeration because each ChildCallback re-enters the
   // registry (Register). recursive_mutex allows the same-thread nesting.
   std::lock_guard<std::recursive_mutex> guard (m_mxRegistry);

   // pRMXSub is a subscription handle (from Model_Open) that has reached its ready
   // state, so its children (fetched by the subscription) are now enumerable.
   // Re-running is safe: Node_Open dedups by identity, so children already opened
   // by an earlier (partial) ready notification are skipped.
   switch (pRMXSub->wClass_Object ())
   {
   case RMAP::MAP::MAP_OBJECT_CLASS_ROOT:
      pRMXSub->Child_Enum ("RMCObject", ChildCallback, this);
      break;

   case RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL:
      pRMXSub->Child_Enum ("RMCObject", ChildCallback, this);
      pRMXSub->Child_Enum ("RMTObject", ChildCallback, this);
      break;

   case RMAP::MAP::MAP_OBJECT_CLASS_TERRESTRIAL:
      pRMXSub->Child_Enum ("RMTObject", ChildCallback, this);
      pRMXSub->Child_Enum ("RMPObject", ChildCallback, this);
      break;

   case RMAP::MAP::MAP_OBJECT_CLASS_PHYSICAL:
      pRMXSub->Child_Enum ("RMPObject", ChildCallback, this);
      break;
   }

   // Mark this node's tier as loaded so the compositor's per-frame Expand is a
   // no-op. onReadyState may still re-run LoadChildren as the subscription
   // advances to a fuller state; Node_Open dedup makes that harmless.
   auto itHandle = m_mpHandleByRMX.find (pRMXSub);

   if (itHandle != m_mpHandleByRMX.end ())
   {
      auto itItem = m_mpRMObject.find (itHandle->second);

      if (itItem != m_mpRMObject.end ())
         itItem->second.bChildrenLoaded = true;
   }
}

void MAPSVC::onReadyState (RMAP::CORE::INOTICE* pNotice)
{
   RMAP::CORE::MODEL_OBJECT* pRMXObject;

   if (pNotice->pCreator == m_pImpl)
   {
      if (m_pImpl->IsReady ())
      {
         ReadyStateEx (READY);
      }
/*
      else if (m_pFabric->ReadyState () == FABRIC::eSTATE::LOGGINGIN_AUTHENTICATE)
      {
         //      this.Emit ("onRCLogAuth");
      }
*/
      else if (m_pImpl->m_pLnG != NULL)
      {
//         ReadyStateEx (NOTREADY);   //      this.Emit ("onRCDisconnected", m_pFabric->pLnG.pSession);
      }
      // else { we just created fabric and ignore this notification }
   }
   else
   {
      pRMXObject = dynamic_cast<RMAP::CORE::MODEL_OBJECT*> (pNotice->pCreator);

      if (pRMXObject != NULL && pRMXObject->IsReady ())
      {
         if (pRMXObject == m_pRMXRoot)
         {
            uint64_t twResult = OBJECTIX_ERROR;
            RMAP::MAP::MAP_OBJECT* pMap_Object = dynamic_cast<RMAP::MAP::MAP_OBJECT*> (pNotice->pCreator);

            twResult = m_pImpl->m_pContainer->Node_Root (m_pImpl->m_twFabricIx, pMap_Object);

            // First ready notification: register the root. Its subscription handle
            // is itself (it was Model_Open'd in ReadyStateEx), so record that and
            // the reverse index. Node_Root returns ERROR on later notifications
            // (root already exists) -- guarded by Register.
            if (twResult != OBJECTIX_ERROR)
            {
               std::lock_guard<std::recursive_mutex> guard (m_mxRegistry);

               Register (twResult, m_pRMXRoot);

               auto it = m_mpRMObject.find (twResult);
               if (it != m_mpRMObject.end ())
                  it->second.pRMXSub = m_pRMXRoot;

               m_mpHandleByRMX[m_pRMXRoot] = twResult;
            }

            // The root's first tier always loads so the scene is never empty;
            // deeper tiers are gated by proximity (Expand). Re-runs as the root
            // subscription advances toward RECOVERED (dedup makes this safe).
            LoadChildren (m_pRMXRoot);
         }
         else
         {
            // A subscription handle (opened by Expand) reached a ready state, so
            // its children are now fetched. Enumerate + Node_Open them. Mirrors the
            // root; load-only, and re-entrant-safe via Node_Open dedup.
            std::lock_guard<std::recursive_mutex> guard (m_mxRegistry);

            auto itHandle = m_mpHandleByRMX.find (pRMXObject);

            if (itHandle != m_mpHandleByRMX.end ())
               LoadChildren (pRMXObject);
         }
      }
   }
}

uint32_t MAPSVC::GetChildCount (RMAP::CORE::MODEL_OBJECT* pRMXObject)
{
   uint32_t nChildren = 0;

   switch (pRMXObject->wClass_Object ())
   {
   case RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL:
      nChildren = dynamic_cast<RMAP::MAP::RMCOBJECT*> (pRMXObject)->Children ();
      break;

   case RMAP::MAP::MAP_OBJECT_CLASS_TERRESTRIAL:
      nChildren = dynamic_cast<RMAP::MAP::RMTOBJECT*> (pRMXObject)->Children ();
      break;

   case RMAP::MAP::MAP_OBJECT_CLASS_PHYSICAL:
      nChildren = dynamic_cast<RMAP::MAP::RMPOBJECT*> (pRMXObject)->Children ();
      break;
   }

   return nChildren;
}

bool MAPSVC::ChildCallback (RMAP::CORE::MODEL_OBJECT* pChild, void* pvParam)
{
   MAPSVC* pMapSvc = static_cast<MAPSVC*> (pvParam);

   if (pMapSvc != NULL && pChild != NULL)
      pMapSvc->OpenChild (pChild);

   return true;
}

uint64_t MAPSVC::OpenChild (RMAP::CORE::MODEL_OBJECT* pChild)
{
   uint64_t qwComposed = OBJECTIX_ERROR;

   RMAP::MAP::MAP_OBJECT* pMap_Object = dynamic_cast<RMAP::MAP::MAP_OBJECT*> (pChild);

   if (pMap_Object != NULL)
   {
      qwComposed = m_pImpl->m_pContainer->Node_Open (OBJECTIX_COMPOSE (pChild->wClass_Parent (), pChild->twParentIx ()), pMap_Object);

      // Node_Open returns ERROR for a child already present (Node_Create dedups by
      // identity), so re-enumeration silently skips existing nodes -- only newly
      // opened children are registered.
      if (qwComposed != OBJECTIX_ERROR)
      {
         Register (qwComposed, pChild);

         // Node_Open pairing: subscribe this child's own model for its lifetime.
         // Complement is Model_Close in Unregister, which runs with Node_Close.
         const char* sID = MapModelId (pChild->wClass_Object ());

         if (sID != NULL)
         {
            RMAP::CORE::MODEL_OBJECT* pRMXOpen = dynamic_cast<RMAP::CORE::MODEL_OBJECT*> (m_pImpl->m_pLnG->Model_Open (sID, std::to_string (pChild->twObjectIx ())));

            std::lock_guard<std::recursive_mutex> guard (m_mxRegistry);

            auto it = m_mpRMObject.find (qwComposed);
            if (it != m_mpRMObject.end ())
               it->second.pRMXOpen = pRMXOpen;
         }
      }
   }

   return qwComposed;
}

void MAPSVC::Register (uint64_t qwComposed, RMAP::CORE::MODEL_OBJECT* pRMXObject)
{
   if (pRMXObject != NULL && qwComposed != OBJECTIX_ERROR)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxRegistry);

      if (m_mpRMObject.find (qwComposed) == m_mpRMObject.end ())
      {
         ITEM Item;

         Item.pRMXObject      = pRMXObject;
         Item.pRMXOpen        = nullptr;   // filled in by OpenChild (Node_Open pairing)
         Item.pRMXSub         = nullptr;   // filled in by Expand when this node is subscribed
         Item.qwComposed      = qwComposed;
         Item.bChildrenLoaded = false;

         m_mpRMObject[qwComposed] = Item;
      }
   }
}

void MAPSVC::Unregister (uint64_t qwComposed)
{
   auto it = m_mpRMObject.find (qwComposed);

   if (it != m_mpRMObject.end ())
   {
      ITEM& Item = it->second;

      if (Item.pRMXSub)
         m_mpHandleByRMX.erase (Item.pRMXSub);

      // Complement of OpenChild's Model_Open -- this is the Node_Close pairing.
      if (Item.pRMXOpen  &&  Item.pRMXOpen != m_pRMXRoot)
         m_pImpl->m_pLnG->Model_Close (Item.pRMXOpen);

      m_mpRMObject.erase (it);
   }
}

void MAPSVC::Expand (uint64_t qwComposed)
{
   std::lock_guard<std::recursive_mutex> guard (m_mxRegistry);

   auto it = m_mpRMObject.find (qwComposed);

   // The compositor may report any node handle; only handles we opened are in the
   // registry. Skip unknown handles, already-loaded tiers, and nodes whose
   // subscription is already in flight (dedup -- Expand is called every frame).
   if (it == m_mpRMObject.end ())
      return;

   ITEM& Item = it->second;

   if (Item.bChildrenLoaded  ||  Item.pRMXSub != NULL  ||  Item.pRMXObject == NULL)
      return;

   const char* sID = MapModelId (Item.pRMXObject->wClass_Object ());

   if (sID != NULL)
   {
      // Reuse OpenChild's Model_Open when present so this node has one subscribe
      // for its lifetime (closed at Unregister / Node_Close). Attach so
      // onReadyState enumerates children. Fallback Model_Open covers the root
      // (opened in ReadyStateEx, pRMXOpen is null) and any node that skipped
      // OpenChild.
      char sHex[1024];
      sprintf (sHex, "Open Model: 0x%llX", qwComposed);
      m_pImpl->m_pContainer->Context ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "MAPSVC", sHex);

      RMAP::CORE::MODEL_OBJECT* pRMXSub = Item.pRMXOpen;

      if (pRMXSub == NULL)
         pRMXSub = dynamic_cast<RMAP::CORE::MODEL_OBJECT*> (m_pImpl->m_pLnG->Model_Open (sID, std::to_string (Item.pRMXObject->twObjectIx ())));

      if (pRMXSub != NULL)
      {
         Item.pRMXSub             = pRMXSub;
         m_mpHandleByRMX[pRMXSub] = qwComposed;

         pRMXSub->Attach (this, false, true);

#if 0
         // If the model resolved immediately (cached), Attach may not deliver a
         // ready notification -- load now. Node_Open dedup keeps this safe if the
         // notification also fires later.
         if (pRMXSub->IsReady ())
            LoadChildren (pRMXSub);
#endif
      }
   }
}

void MAPSVC::Collapse (uint64_t qwComposed)
{
   std::lock_guard<std::recursive_mutex> guard (m_mxRegistry);

   auto it = m_mpRMObject.find (qwComposed);

   // Complement of Expand: unknown handles, the root (first tier always stays),
   // and nodes that never streamed children are no-ops. Collapse is called every
   // frame for out-of-view nodes, so the skipped cases are the per-frame fast path.
   if (it != m_mpRMObject.end ())
   {
      ITEM& Item = it->second;

      bool bRoot = (Item.pRMXSub == m_pRMXRoot  ||  Item.pRMXObject == m_pRMXRoot);
      bool bIdle = (Item.pRMXSub == nullptr  &&  !Item.bChildrenLoaded);
      RMAP::CORE::MODEL_OBJECT* pRMXSub = nullptr;

      if (!bRoot  &&  !bIdle)
      {
         // Detach the Expand subscription first so a late onReadyState cannot
         // LoadChildren into a tree we are tearing down. Model_Close of pRMXOpen
         // waits for Unregister / Node_Close (the node itself stays). Close
         // pRMXSub now only when it is a second Model_Open, not the OpenChild handle.
         if (Item.pRMXSub)
         {
            char sHex[1024];
            sprintf (sHex, "Close Model: 0x%llX", qwComposed);
            m_pImpl->m_pContainer->Context ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "MAPSVC", sHex);

            Item.pRMXSub->Detach (this);
            m_mpHandleByRMX.erase (Item.pRMXSub);

            if (Item.pRMXSub != Item.pRMXOpen)
               pRMXSub = Item.pRMXSub;

            Item.pRMXSub = nullptr;
         }

         NODE* pNode = m_pImpl->m_pContainer->Node_Find (qwComposed);

         if (pNode)
         {
            while (pNode->Node_Count () > 0)
            {
               NODE* pChild = pNode->Child (0);

               if (pChild  &&  pChild->Map_Object ())
               {
                  uint64_t qwChild = OBJECTIX_COMPOSE (pChild->Map_Object ()->m_wClass, pChild->ObjectIx ());

                  // Descendants first: unsubscribe their models and close *their*
                  // children with composed handles (NODE's destructor closes
                  // children by raw ObjectIx, which misses the composed-key table).
                  Collapse (qwChild);
                  Unregister (qwChild);

                  if (!m_pImpl->m_pContainer->Node_Close (qwChild))
                     break;
               }
               else
                  break;
            }
         }

         if (pRMXSub)
            m_pImpl->m_pLnG->Model_Close (pRMXSub);

         Item.bChildrenLoaded = false;
      }
   }
}
