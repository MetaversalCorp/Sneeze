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
   MAPSVC* pMapSvc = (MAPSVC*)pvParam;
   RMAP::MAP::RMCOBJECT* pRMCObject = dynamic_cast<RMAP::MAP::RMCOBJECT*> (pChild);
   
//   pMapSvc->AddItem (pRMXItem->hParent, TVI_LAST, pRMCObject->Name ().wsRMCObjectId (), pRMCObject);

   return true;
}

bool RMTObjectCallback (RMAP::CORE::MODEL_OBJECT* pChild, void* pvParam)
{
   MAPSVC* pMapSvc = (MAPSVC*)pvParam;
   RMAP::MAP::RMTOBJECT* pRMTObject = dynamic_cast<RMAP::MAP::RMTOBJECT*> (pChild);

//   pMapSvc->AddItem (pRMXItem->hParent, TVI_LAST, pRMTObject->Name ().wsRMTObjectId (), pRMTObject);

   return true;
}

bool RMPObjectCallback (RMAP::CORE::MODEL_OBJECT* pChild, void* pvParam)
{
   MAPSVC* pMapSvc = (MAPSVC*)pvParam;
   RMAP::MAP::RMPOBJECT* pRMPObject = dynamic_cast<RMAP::MAP::RMPOBJECT*> (pChild);

//   pMapSvc->AddItem (pRMXItem->hParent, TVI_LAST, pRMPObject->Name ().wsRMPObjectId (), pRMPObject);

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
   Impl (const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map) :
      m_wClass_Map (wClass_Map),
      m_twObjectIx_Map (twObjectIx_Map)
   {
      RMAP::CORE::APP* pCore = RMAP::CORE::APP::GetInstance ();

      if ((m_pRequire = pCore->Require ("Map", sService, sNamespace)) != NULL)
      {
         if ((m_pLnG = pCore->LnG_Open (sNamespace, "sService",sConnect, "")) != NULL)
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

private:
   RMAP::CORE::APP::REQUIRE*           m_pRequire;
};

/*******************************************************************************************************************************
**                                                     CLASS (MAPSVC)                                                         **
*******************************************************************************************************************************/

MAPSVC::MAPSVC (const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map) :
   m_pImpl (new Impl (sNamespace, sService, sConnect, wClass_Map, twObjectIx_Map)),
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

#if 0
void MAPSVC::LoadChildren (RMAP::CORE::MODEL_OBJECT* pRMXObject, HTREEITEM hParent)
{
   RMXITEM RMXItem;
   std::string sKey;
   ITEM* pItem;

   RMXItem.hParent = hParent;
   RMXItem.pMapSvc = this;

   switch (pRMXObject->wClass_Object ())
   {
   case RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_CELESTIAL:
      pRMXObject->Child_Enum ("RMCObject", RMCObjectCallback, &RMXItem);
      pRMXObject->Child_Enum ("RMTObject", RMTObjectCallback, &RMXItem);
      break;

   case RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_TERRESTRIAL:
      pRMXObject->Child_Enum ("RMTObject", RMTObjectCallback, &RMXItem);
      pRMXObject->Child_Enum ("RMPObject", RMPObjectCallback, &RMXItem);
      break;

   case RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_PHYSICAL:
      pRMXObject->Child_Enum ("RMPObject", RMPObjectCallback, &RMXItem);
      break;
   }

   sKey = std::to_string (pRMXObject->wClass_Object ()) + "-" + std::to_string (pRMXObject->twObjectIx ());

   auto it = m_mpRMObject.find (sKey);

   if (it != m_mpRMObject.end ())
   {
      pItem = &it->second;
      pItem->bChildrenLoaded = true;
   }

   TreeView_SortChildren (m_hwndTree, hParent, 0);

   SendMessage (m_hwndTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)hParent);
}
#endif

bool MAPSVC::GetObjectId (RMAP::MAP::MAP_OBJECT* pMap_Object, std::wstring& wsObjectId)
{
   bool bResult = true;
   RMAP::MAP::MAP_OBJECT_POD Pod;

   pMap_Object->GetPOD (Pod);
   
   uint16_t* pwBuffer = Pod.Name.wsName;

   while (*pwBuffer)
   {
      wsObjectId.push_back (static_cast<wchar_t>(*pwBuffer++));
   }

   return bResult;
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
         ReadyStateEx (NOTREADY);   //      this.Emit ("onRCDisconnected", m_pFabric->pLnG.pSession);
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
            if (GetObjectId (dynamic_cast<RMAP::MAP::MAP_OBJECT*> (pNotice->pCreator), wsObjectId))
            {
//               HTREEITEM hRoot = AddItem (TVI_ROOT, TVI_ROOT, wsObjectId, m_pRMXRoot);

//               LoadChildren (m_pRMXRoot, hRoot);
            }
         }
         else
         {
            sKey = std::to_string (pRMXObject->wClass_Object ()) + "-" + std::to_string (pRMXObject->twObjectIx ());

            auto it = m_mpRMObject.find (sKey);

            if (it != m_mpRMObject.end () && !it->second.bChildrenLoaded)
            {
//               LoadChildren (pRMXObject, it->second.hTreeItem);
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

#if 0
HTREEITEM MAPSVC::AddItem (HTREEITEM hParent, HTREEITEM hInsertAfter, std::wstring wsText, RMAP::CORE::MODEL_OBJECT* pRMXObject)
{
   TVINSERTSTRUCT tvi = {0};
   ITEM Item;
   std::string sKey;
   ITEM* pItem;

   Item.hTreeItem       = NULL;
   Item.pRMXObject      = pRMXObject;
   Item.bChildrenLoaded = false;
   Item.bAttached       = false;

   sKey = std::to_string (pRMXObject->wClass_Object ()) + "-" + std::to_string (pRMXObject->twObjectIx ());

   pItem = &m_mpRMObject.insert ({ sKey, Item }).first->second;

   if (wsText.empty ())
      wsText = L"<EMPTY_NAME>";

   tvi.hParent          = hParent;
   tvi.hInsertAfter     = hInsertAfter;
   tvi.item.mask        = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
   tvi.item.pszText     = const_cast<PWSTR> (wsText.c_str ());
   tvi.item.cChildren   = (GetChildCount (pRMXObject) > 0) ? 1 : 0;
   tvi.item.lParam      = reinterpret_cast<LPARAM> (pItem);

   pItem->hTreeItem = TreeView_InsertItem (m_hwndTree, &tvi);

   return pItem->hTreeItem;
}
#endif

#if 0
void MAPSVC::PanelClear ()
{
   SetDlgItemText (m_hWnd, IDC_MAP_NAME, L"");
   SetDlgItemText (m_hWnd, IDC_MAP_TYPE, L"");
   SetDlgItemText (m_hWnd, IDC_MAP_ID,   L"");
}

void MAPSVC::PanelUpdateCommon (PCWSTR pcwszObject, uint16_t wClass, uint64_t twObjectIx, std::wstring wsObjectId, RMAP::MAP::TYPE pType, RMAP::MAP::RESOURCE pResource, RMAP::MAP::TRANSFORM pTransform, RMAP::MAP::BOUND pBound)
{
   std::wstring wsType;

   wsType = pcwszObject;

   if (wClass == RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_CELESTIAL)
      wsType += g_pwcszRMCTypes[pType.bType];
   else if (wClass == RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_TERRESTRIAL)
      wsType += g_pwcszRMTTypes[pType.bType];

   SetDlgItemText (m_hWnd, IDC_MAP_NAME, const_cast <PWSTR> (wsObjectId.c_str ()));
   SetDlgItemText (m_hWnd, IDC_MAP_TYPE, const_cast <PWSTR> (wsType.c_str ()));
   SetDlgItemText (m_hWnd, IDC_MAP_ID, const_cast <PWSTR> (std::to_wstring (twObjectIx).c_str ()));

   SetDlgItemTextA (m_hWnd, IDC_MAP_RES_NAME, const_cast <PSTR> (pResource.sName ().c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_RES_REF, const_cast <PSTR> (pResource.sReference ().c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_RES_RES, const_cast <PSTR> (std::to_string (pResource.qwResource ()).c_str ()));

   SetDlgItemTextA (m_hWnd, IDC_MAP_POS_X, const_cast <PSTR> (std::to_string (pTransform.vPosition.dX).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_POS_Y, const_cast <PSTR> (std::to_string (pTransform.vPosition.dY).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_POS_Z, const_cast <PSTR> (std::to_string (pTransform.vPosition.dZ).c_str ()));

   SetDlgItemTextA (m_hWnd, IDC_MAP_ROT_X, const_cast <PSTR> (std::to_string (pTransform.qRotation.dX).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_ROT_Y, const_cast <PSTR> (std::to_string (pTransform.qRotation.dY).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_ROT_Z, const_cast <PSTR> (std::to_string (pTransform.qRotation.dZ).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_ROT_W, const_cast <PSTR> (std::to_string (pTransform.qRotation.dW).c_str ()));

   SetDlgItemTextA (m_hWnd, IDC_MAP_SCL_X, const_cast <PSTR> (std::to_string (pTransform.vScale.dX).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_SCL_Y, const_cast <PSTR> (std::to_string (pTransform.vScale.dY).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_SCL_Z, const_cast <PSTR> (std::to_string (pTransform.vScale.dZ).c_str ()));

   SetDlgItemTextA (m_hWnd, IDC_MAP_BND_X, const_cast <PSTR> (std::to_string (pBound.dX).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_BND_Y, const_cast <PSTR> (std::to_string (pBound.dY).c_str ()));
   SetDlgItemTextA (m_hWnd, IDC_MAP_BND_Z, const_cast <PSTR> (std::to_string (pBound.dZ).c_str ()));
}

void MAPSVC::PanelUpdate (RMAP::CORE::MODEL_OBJECT* pRMXObject)
{
   RMAP::MAP::RMCOBJECT* pRMCObject;
   RMAP::MAP::RMPOBJECT* pRMPObject;
   RMAP::MAP::RMTOBJECT* pRMTObject;

   switch (pRMXObject->wClass_Object ())
   {
   case RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_CELESTIAL:
      pRMCObject = dynamic_cast<RMAP::MAP::RMCOBJECT*> (pRMXObject);

      PanelUpdateCommon (L"RMCOBJECT::", pRMCObject->wClass_Object (), pRMCObject->twObjectIx (), pRMCObject->Name ().wsRMCObjectId (), pRMCObject->Type (), pRMCObject->Resource (), pRMCObject->Transform (), pRMCObject->Bound ());
      break;

   case RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_PHYSICAL:
      pRMPObject = dynamic_cast<RMAP::MAP::RMPOBJECT*> (pRMXObject);

      PanelUpdateCommon (L"RMPOBJECT::", pRMPObject->wClass_Object (), pRMPObject->twObjectIx (), pRMPObject->Name ().wsRMPObjectId (), pRMPObject->Type (), pRMPObject->Resource (), pRMPObject->Transform (), pRMPObject->Bound ());
      break;

   case RMAP::MAP::MAP_DATA::MAP_OBJECT_CLASS_TERRESTRIAL:
      pRMTObject = dynamic_cast<RMAP::MAP::RMTOBJECT*> (pRMXObject);

      PanelUpdateCommon (L"RMTOBJECT::", pRMTObject->wClass_Object (), pRMTObject->twObjectIx (), pRMTObject->Name ().wsRMTObjectId (), pRMTObject->Type (), pRMTObject->Resource (), pRMTObject->Transform (), pRMTObject->Bound ());
      break;
   }
}
#endif