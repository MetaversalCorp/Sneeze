#ifndef SNEEZE_SCENE_MAPSVC_H
#define SNEEZE_SCENE_MAPSVC_H

// Helpers that produce an RMCOBJECT -- the flat wire form of a SOM node (any
// class: root, celestial, terrestrial, physical, panel, light). RMCOBJECT and
// nlohmann::json are supplied by the umbrella <Sneeze.h> (force-included ahead
// of every translation unit via the precompiled header).

namespace SNEEZE
{
   class MAPSVC : public RMAP::CORE::NOTIFICATION
   {
   public:
      enum eSTATE
      {
         NOTREADY = 0,
         READY = 1
      };

   public:
      struct ITEM
      {
         RMAP::CORE::MODEL_OBJECT*        pRMXObject;
         bool                             bChildrenLoaded;
         bool                             bAttached;
      };

   public:
      MAPSVC (const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map);
      ~MAPSVC ();

   public:
      void Notify (RMAP::CORE::INOTICE* pNotice) override;

      void onReadyState (RMAP::CORE::INOTICE* pNotice);

      void ReadyStateEx (int nReadyState);

      bool      GetObjectId (RMAP::MAP::MAP_OBJECT* pMap_Object, std::wstring& wsObjectId);
  //    void      LoadChildren (RMAP::CORE::MODEL_OBJECT* pRMXObject, HTREEITEM hParent);
      uint32_t  GetChildCount (RMAP::CORE::MODEL_OBJECT* pRMXObject);
//      HTREEITEM AddItem (HTREEITEM hParent, HTREEITEM hInsertAfter, std::wstring wsText, RMAP::CORE::MODEL_OBJECT* pRMXObject);

#if 0
      void PanelUpdateCommon (PCWSTR pcwszObject, uint16_t wClass, uint64_t twObjectIx, std::wstring wsObjectId, RMAP::MAP::TYPE pType, RMAP::MAP::RESOURCE pResource, RMAP::MAP::TRANSFORM pTransform, RMAP::MAP::BOUND pBound);
      void PanelUpdate (RMAP::CORE::MODEL_OBJECT* pRMXObject);
      void PanelClear ();
#endif
   private:
      class Impl;
      Impl* m_pImpl;

      RMAP::CORE::MODEL_OBJECT*     m_pRMXRoot;
      std::map<std::string, ITEM>   m_mpRMObject;
   };
}

#endif 