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
      MAPSVC (CONTAINER* pContainer, uint64_t twFabricIx, const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map);
      ~MAPSVC ();

   public:
      void Notify (RMAP::CORE::INOTICE* pNotice) override;

      void onReadyState (RMAP::CORE::INOTICE* pNotice);

      void ReadyStateEx (int nReadyState);

   private:
      void      LoadChildren (RMAP::CORE::MODEL_OBJECT* pRMXObject);
      uint32_t  GetChildCount (RMAP::CORE::MODEL_OBJECT* pRMXObject);

   private:
      class Impl;
      Impl* m_pImpl;

      RMAP::CORE::MODEL_OBJECT*     m_pRMXRoot;
      std::map<std::string, ITEM>   m_mpRMObject;
   };
}

#endif 