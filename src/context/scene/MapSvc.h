#ifndef SNEEZE_SCENE_MAPSVC_H
#define SNEEZE_SCENE_MAPSVC_H

// Helpers that produce an RMCOBJECT -- the flat wire form of a SOM node (any
// class: root, celestial, terrestrial, physical, panel, light). RMCOBJECT and
// nlohmann::json are supplied by the umbrella <Sneeze.h> (force-included ahead
// of every translation unit via the precompiled header).

#include <cstdint>
#include <map>
#include <mutex>

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
         RMAP::CORE::MODEL_OBJECT*        pRMXObject;        // node object from the parent's Child_Enum (PARTIAL): source of class/objectix + the Node_Open that created the node
         RMAP::CORE::MODEL_OBJECT*        pRMXOpen;          // Model_Open at OpenChild; Model_Close at Unregister (Node_Close pairing)
         RMAP::CORE::MODEL_OBJECT*        pRMXSub;           // Expand attach of pRMXOpen (or a Model_Open if OpenChild had none); null until expanded
         uint64_t                         qwComposed;        // composed OBJECTIX handle (NODE::ObjectIx)
         bool                             bChildrenLoaded;   // one child level streamed in
      };

   public:
      MAPSVC (CONTAINER* pContainer, uint64_t twFabricIx, const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map);
      ~MAPSVC ();

   public:
      void Notify (RMAP::CORE::INOTICE* pNotice) override;

      void onReadyState (RMAP::CORE::INOTICE* pNotice);

      void ReadyStateEx (int nReadyState);

      // Proximity-driven lazy loading entry point (reached via CONTAINER::Node_Expand
      // from the compositor after traversal). Subscribes the node's map model (LnG
      // Model_Open) so its children get fetched, then enumerates + Node_Opens them
      // when the subscription reaches its ready state (mirrors the root). Idempotent
      // -- an already-expanded or unknown handle is ignored.
      void Expand (uint64_t qwComposed);

      // Complement of Expand: unsubscribe the node's map model (LnG Model_Close) and
      // Node_Close its streamed-in children (recursively, descendants first). The
      // node itself stays so a later Expand can refill it. The root is never
      // collapsed (first tier always remains). Idempotent -- unknown, never-expanded,
      // or root handles are ignored.
      void Collapse (uint64_t qwComposed);

      // True when this node's composed handle is in the map registry -- i.e. the
      // map service opened it. The compositor confines proximity expand/collapse
      // to map-managed nodes, exempting WASM-injected / static content.
      bool IsRegistered (uint64_t qwComposed);

   private:
      // Child_Enum callback: turns each enumerated map-service child into a node
      // (Node_Open) and registers it. Static so it matches RMAP's fnModelObjectEnum
      // function-pointer type; the MAPSVC instance arrives through pvParam.
      static bool ChildCallback (RMAP::CORE::MODEL_OBJECT* pChild, void* pvParam);

      uint64_t  OpenChild     (RMAP::CORE::MODEL_OBJECT* pChild);
      void      Register      (uint64_t qwComposed, RMAP::CORE::MODEL_OBJECT* pRMXObject);
      void      Unregister    (uint64_t qwComposed);
      void      LoadChildren  (RMAP::CORE::MODEL_OBJECT* pRMXSub);
      uint32_t  GetChildCount (RMAP::CORE::MODEL_OBJECT* pRMXObject);

   private:
      class Impl;
      Impl* m_pImpl;

      RMAP::CORE::MODEL_OBJECT*                        m_pRMXRoot;

      // Registry of opened map nodes, keyed by composed OBJECTIX handle (the value
      // NODE::ObjectIx reports and the compositor passes to Expand). The reverse
      // index resolves the RMX model pointer (delivered by onReadyState) back to
      // its handle. Guarded because Expand runs on the compositor thread while
      // onReadyState / ChildCallback run on RMAP threads.
      std::map<uint64_t, ITEM>                        m_mpRMObject;
      std::map<RMAP::CORE::MODEL_OBJECT*, uint64_t>   m_mpHandleByRMX;
      std::recursive_mutex                            m_mxRegistry;
   };
}

#endif 