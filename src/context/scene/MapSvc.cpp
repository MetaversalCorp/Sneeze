/*******************************************************************************************************************************
**                                                                                                                            **
**                                      MVSAMPLE_cpp : MAPSVC.cpp                                                             **
**                                                                                                                            **
********************************************************************************************************************************
**                                                                                                                            **
*******************************************************************************************************************************/

#include "MapSvc.h"

using namespace SNEEZE;

bool RMCObjectCallback (RMAP::CORE::MODEL_OBJECT* pChild, void* pvParam)
{
   CONTAINER* pContainer = (CONTAINER*)pvParam;
   uint64_t twResult = OBJECTIX_ERROR;
   RMAP::MAP::MAP_OBJECT* pMap_Object = dynamic_cast<RMAP::MAP::MAP_OBJECT*> (pChild);

   twResult = pContainer->Node_Open (OBJECTIX_COMPOSE (pChild->wClass_Parent (), pChild->twParentIx ()), pMap_Object);

   return true;
}

bool RMTObjectCallback (RMAP::CORE::MODEL_OBJECT* pChild, void* pvParam)
{
   CONTAINER* pContainer = (CONTAINER*)pvParam;
   uint64_t twResult = OBJECTIX_ERROR;
   RMAP::MAP::MAP_OBJECT* pMap_Object = dynamic_cast<RMAP::MAP::MAP_OBJECT*> (pChild);

   twResult = pContainer->Node_Open (OBJECTIX_COMPOSE (pChild->wClass_Parent (), pChild->twParentIx ()), pMap_Object);

   return true;
}

bool RMPObjectCallback (RMAP::CORE::MODEL_OBJECT* pChild, void* pvParam)
{
   CONTAINER* pContainer = (CONTAINER*)pvParam;
   uint64_t twResult = OBJECTIX_ERROR;
   RMAP::MAP::MAP_OBJECT* pMap_Object = dynamic_cast<RMAP::MAP::MAP_OBJECT*> (pChild);

   twResult = pContainer->Node_Open (OBJECTIX_COMPOSE (pChild->wClass_Parent (), pChild->twParentIx ()), pMap_Object);

   return true;
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
   for (auto& elem : m_mpRMObject)
   {
      if (elem.second.bAttached)
         elem.second.pRMXObject->Detach (this);

#if 0    // We don't do a Model_Open and just the collection that already exists
      if (m_pRMXRoot != elem.second.pRMXObject)
         m_pLnGMap->Model_Close (elem.second.pRMXObject);
#endif
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
      m_pRMXRoot = dynamic_cast <RMAP::CORE::MODEL_OBJECT*> (m_pImpl->m_pLnG->Model_Open (sID_Model, std::to_string (m_pImpl->m_twObjectIx_Map)));
      m_pRMXRoot->Attach (this, false, true);
   }
}

void MAPSVC::LoadChildren (RMAP::CORE::MODEL_OBJECT* pRMXObject)
{
   std::string sKey;
   ITEM* pItem;

   switch (pRMXObject->wClass_Object ())
   {
   case RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL:
      pRMXObject->Child_Enum ("RMCObject", RMCObjectCallback, m_pImpl->m_pContainer);
      pRMXObject->Child_Enum ("RMTObject", RMTObjectCallback, m_pImpl->m_pContainer);
      break;

   case RMAP::MAP::MAP_OBJECT_CLASS_TERRESTRIAL:
      pRMXObject->Child_Enum ("RMTObject", RMTObjectCallback, m_pImpl->m_pContainer);
      pRMXObject->Child_Enum ("RMPObject", RMPObjectCallback, m_pImpl->m_pContainer);
      break;

   case RMAP::MAP::MAP_OBJECT_CLASS_PHYSICAL:
      pRMXObject->Child_Enum ("RMPObject", RMPObjectCallback, m_pImpl->m_pContainer);
      break;
   }

   sKey = std::to_string (pRMXObject->wClass_Object ()) + "-" + std::to_string (pRMXObject->twObjectIx ());

   auto it = m_mpRMObject.find (sKey);

   if (it != m_mpRMObject.end ())
   {
      pItem = &it->second;
      pItem->bChildrenLoaded = true;
   }
}

void MAPSVC::onReadyState (RMAP::CORE::INOTICE* pNotice)
{
   RMAP::CORE::MODEL_OBJECT* pRMXObject;
   std::string sKey;
   std::wstring wsObjectId;

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

            LoadChildren (m_pRMXRoot);
         }
         else
         {
            sKey = std::to_string (pRMXObject->wClass_Object ()) + "-" + std::to_string (pRMXObject->twObjectIx ());

            auto it = m_mpRMObject.find (sKey);

            if (it != m_mpRMObject.end () && !it->second.bChildrenLoaded)
            {
#if 0
               uint64_t twResult = OBJECTIX_ERROR;
               RMAP::MAP::MAP_OBJECT* pMap_Object = dynamic_cast<RMAP::MAP::MAP_OBJECT*> (pNotice->pCreator);

               twResult = m_pImpl->m_pContainer->Node_Open (pMap_Object);

               LoadChildren (pRMXObject);
#endif
            }
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
