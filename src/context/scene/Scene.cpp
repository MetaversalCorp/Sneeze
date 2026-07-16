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

#include <Sneeze.h>

#include "Map_Object.h"
#include "RmcObject.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>

using namespace SNEEZE;

// Default brightness for the primary fabric's scene-global lights: used for the
// per-block fallback when an authored "Ambient"/"Directional" omits fBrightness,
// and for the whole-scene fallback when a fabric authors no global light at all
// (so a scene is never dark by accident).
#define SCENE_DEFAULT_BRIGHTNESS 0.5f

// Default colour ("RRGGBB" hex) for a scene-global light whose "fColor" is
// omitted -- neutral white, so an unlit-colour light does not tint the scene.
#define SCENE_DEFAULT_LIGHT_COLOR "FFFFFF"

// JSON keys for the primary fabric's "Primary" presentation block. Keys that
// share a spelling across sub-objects (Rotation, fBrightness, fColor) are kept
// as separate constants per context, so each can change independently later.
#define PRIMARY_KEY_BLOCK                            "Primary"
#define PRIMARY_KEY_CAMERA                           "Camera"
#define PRIMARY_KEY_CAMERA_POSITION                  "Position"
#define PRIMARY_KEY_CAMERA_ROTATION                  "Rotation"
#define PRIMARY_KEY_BACKGROUND                       "rgbBackground"
#define PRIMARY_KEY_AMBIENT                          "Ambient"
#define PRIMARY_KEY_AMBIENT_BRIGHTNESS               "fBrightness"
#define PRIMARY_KEY_AMBIENT_COLOR                    "fColor"
#define PRIMARY_KEY_DIRECTIONAL                      "Directional"
#define PRIMARY_KEY_DIRECTIONAL_BRIGHTNESS           "fBrightness"
#define PRIMARY_KEY_DIRECTIONAL_COLOR                "fColor"
#define PRIMARY_KEY_DIRECTIONAL_ROTATION             "Rotation"

// The "fabric couldn't load" error page: a single panel node injected via
// Branch_Add when a PRIMARY fabric fails to fetch/parse/open. The node JSON
// keys mirror the SOM node schema (kept as their own constants per the
// per-context key convention); the page is sized by aspect only (Bound.Max)
// and carries a constant RML document.
#define ERROR_KEY_HEAD          "Head"
#define ERROR_KEY_HEAD_SELF     "Self"
#define ERROR_KEY_NAME          "Name"
#define ERROR_KEY_BOUND         "Bound"
#define ERROR_KEY_BOUND_MAX     "Max"

#define ERROR_PAGE_NAME         "Error"
#define ERROR_PAGE_ASPECT_W     1.6
#define ERROR_PAGE_ASPECT_H     0.9
#define ERROR_PAGE_BG_R         0.06f
#define ERROR_PAGE_BG_G         0.07f
#define ERROR_PAGE_BG_B         0.09f

#define ERROR_PAGE_DOCUMENT \
   "<rml>" \
   "<head><style>" \
   "body { width: 100%; height: 100%; font-family: Inter; color: #e9eef6; }" \
   "#card {" \
   "   position: absolute; left: 8%; top: 8%; width: 84%; height: 84%;" \
   "   padding: 36px 36px;" \
   "   background-color: rgba(20, 22, 28, 232);" \
   "   border-width: 1px; border-color: rgba(255, 120, 120, 60);" \
   "   border-radius: 18px;" \
   "}" \
   ".title { display: block; font-size: 26px; font-weight: 600; color: #ff8a8a; margin: 0 0 18px 0; }" \
   ".body  { display: block; font-size: 16px; color: #c4ccd8; }" \
   "</style></head>" \
   "<body>" \
   "<div id='card'>" \
   "<span class='title'>This fabric couldn't load</span>" \
   "<span class='body'>The metaverse browser was unable to load this location. It may be offline, moved, or not a valid fabric.</span>" \
   "</div>" \
   "</body>" \
   "</rml>"

// Parses an "RRGGBB" hex colour (optionally 0x- or #-prefixed) into an RGB
// triple in [0,1]. Malformed input yields black.
static void HexColor (const std::string& sHex, RGB& rgb)
{
   std::string sBody = sHex;

   if (sBody.rfind ("0x", 0) == 0  ||  sBody.rfind ("0X", 0) == 0)
      sBody = sBody.substr (2);
   else if (!sBody.empty ()  &&  sBody[0] == '#')
      sBody = sBody.substr (1);

   uint32_t nColor = static_cast<uint32_t> (strtoul (sBody.c_str (), nullptr, 16));

   rgb.fR = ((nColor >> 16) & 0xFF) / 255.0f;
   rgb.fG = ((nColor >>  8) & 0xFF) / 255.0f;
   rgb.fB = ( nColor        & 0xFF) / 255.0f;
}

// The world-space direction an object faces at rotation q: the identity forward
// (+X, per the Z-up identity contract) rotated by q -- i.e. column 0 of q's
// rotation matrix. A directional light is aimed exactly like a spot node, so its
// travel vector is derived from an authored quaternion the same way.
static VEC3 ForwardFromQuat (const QUAT& q)
{
   VEC3 vForward;

   vForward.dX = 1.0 - 2.0 * (q.dY * q.dY + q.dZ * q.dZ);
   vForward.dY =       2.0 * (q.dX * q.dY + q.dW * q.dZ);
   vForward.dZ =       2.0 * (q.dX * q.dZ - q.dW * q.dY);

   return vForward;
}

// ---------------------------------------------------------------------------
// MSF_FETCH — file-local helper that handles the async MSF file fetch.
// Delegates to SCENE::Impl's callback methods.
// ---------------------------------------------------------------------------

class MSF_FETCH : public IFILE
{
public:
   MSF_FETCH (SCENE* pScene, NODE* pNode_Attach) :
      m_pScene       (pScene),
      m_pNode_Attach (pNode_Attach),
      m_pFile        (nullptr)
   {
   }

   bool Initialize (CONTAINER* pContainer, const std::string& sUrl)
   {
      m_pFile = pContainer->Cache ()->File_Open (sUrl, this);

      return (m_pFile != nullptr);
   }

   ~MSF_FETCH ()
   {
      if (m_pFile)
      {
         m_pFile->Close ();
         m_pFile = nullptr;
      }
   }

   void OnFileReady  (SNEEZE::FILE* pFile) override { m_pScene->OnMsfReady  (m_pNode_Attach, pFile); delete this; }
   void OnFileFailed (SNEEZE::FILE* pFile) override { m_pScene->OnMsfFailed (m_pNode_Attach, pFile); delete this; }

   SCENE*         m_pScene;
   NODE*          m_pNode_Attach;
   SNEEZE::FILE*  m_pFile;
};

// ---------------------------------------------------------------------------
// SCENE::Impl
// ---------------------------------------------------------------------------

class SCENE::Impl
{
public:
   Impl (SCENE* pScene, CONTEXT* pContext) :
      m_pScene            (pScene),
      m_pContext          (pContext),
      m_pFabric_Root      (nullptr),
      m_pNode_Primary     (nullptr),
      m_twFabricIx_Next   (0),
      m_rgbaBackground    ({ 0.0f, 0.0f, 0.0f, 1.0f }),
      m_bBackdrop_Changed (false)
   {
   }

   bool Initialize (const std::string& sUrl)
   {
      return Fabric_Root_Create (sUrl);
   }

   ~Impl ()
   {
      Fabric_Root_Destroy ();
   }

// -----------------------------------------------------------------------
// Root fabric lifecycle
// -----------------------------------------------------------------------

   bool Fabric_Root_Create (const std::string& sUrl)
   {
      bool bResult = false;

      RMCOBJECT RMCObject;
      uint64_t twObjectIx;

      // Each fresh load starts from the default backdrop -- black; the primary
      // fabric overrides it afterwards.
      Background ({ 0.0f, 0.0f, 0.0f, 1.0f });

      if ((m_pFabric_Root = Fabric_Open (nullptr, nullptr, sUrl)) != nullptr)
      {
         CONTAINER* pContainer = m_pFabric_Root->Container ();

         RmcObject_Init (RMCObject);
         RMCObject.Head.Self.qwComposed = OBJECTIX_COMPOSE (MAP_OBJECT::MAP_OBJECT_CLASS_ROOT, OBJECTIX_IDENTITY);

         if ((twObjectIx = pContainer->Node_Root (m_pFabric_Root->FabricIx (), &RMCObject)) != OBJECTIX_ERROR)
         {
            uint64_t twRootIx = twObjectIx;

            RmcObject_Init (RMCObject);
            RMCObject.Head.Parent.qwComposed = twRootIx;
            RMCObject.Head.Self  .qwComposed = OBJECTIX_COMPOSE (MAP_OBJECT::MAP_OBJECT_CLASS_ROOT, OBJECTIX_IDENTITY);
            RMCObject.Type.bSubtype = 255;
            strncpy (RMCObject.Resource.sReference, sUrl.c_str (), sizeof (RMCObject.Resource.sReference) - 1);

            if ((twObjectIx = pContainer->Node_Open (&RMCObject)) != OBJECTIX_ERROR)
            {
               m_pNode_Primary = pContainer->Node_Find (twObjectIx);

               bResult = true;
            }
         }
      }

      return bResult;
   }

   void Fabric_Root_Destroy ()
   {
      if (m_pFabric_Root)
      {
         m_pNode_Primary = nullptr;

         m_pFabric_Root = Fabric_Close (m_pFabric_Root);
      }

      // Deleting the root fabric triggers a cascade: deleting its nodes will
      // recursively delete all child nodes. When a node is an attachment
      // point, the fabric attached to it will also be deleted. By the time
      // the root fabric is fully deleted, all descendant fabrics (including
      // the primary) should have been deleted as well.

      if (!m_umpFabric.empty ())
         m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "SCENE", "Leaked " + std::to_string (m_umpFabric.size ()) + " fabric(s)");
      m_umpFabric.clear ();

      m_twFabricIx_Next = 0;
   }

// -----------------------------------------------------------------------
// MSF loaded — open container, create fabric, begin WASM fetches
// -----------------------------------------------------------------------

   void Fabric_Spawn (NODE* pNode_Attach, const std::string& sUrl)
   {
      // we're going to need a way to cancel this, and 
      // we're going to need to return a value

      if (!sUrl.empty ())
      {
         MSF_FETCH* pMsf_Fetch = new MSF_FETCH (m_pScene, pNode_Attach);

         if (!pMsf_Fetch->Initialize (m_pFabric_Root->Container (), sUrl))
         {
            delete pMsf_Fetch;

            m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "SCENE", "Failed to start MSF fetch for " + sUrl);
         }
      }
   }

   // Applies the primary fabric's "Primary" presentation block: the initial
   // camera pose (absolute world position metres + orientation quaternion), the
   // background colour ("RRGGBB" hex), and the two scene-global lights (ambient
   // and the directional "sun"). Every key is optional. Global lighting always
   // resolves, even absent a "Primary" block: with ambient and directional now
   // scene properties rather than nodes, a fabric commonly carries no lights and
   // leans on this setup, so if neither is authored the scene is seeded with a
   // full white ambient and is never dark by accident. A fabric that authors
   // either (even to zero brightness) takes full ownership of its lighting.
   void Primary_Apply (MSF* pMsf)
   {
      nlohmann::json jPayload = pMsf->Payload ();

      SCENE_LIGHT Scene_Light_Ambient;         // struct default: white, brightness 0
      SCENE_LIGHT Scene_Light_Directional;     // struct default: white, brightness 0, +X (identity forward)

      bool bAmbient     = false;
      bool bDirectional = false;

      if (jPayload.is_object ()  &&  jPayload.contains (PRIMARY_KEY_BLOCK)  &&  jPayload[PRIMARY_KEY_BLOCK].is_object ())
      {
         const nlohmann::json& jPrimary = jPayload[PRIMARY_KEY_BLOCK];

         if (jPrimary.contains (PRIMARY_KEY_CAMERA)  &&  jPrimary[PRIMARY_KEY_CAMERA].is_object ())
         {
            const nlohmann::json& jCamera = jPrimary[PRIMARY_KEY_CAMERA];
            VIEWPORT::CAMERA Camera;

            if (jCamera.contains (PRIMARY_KEY_CAMERA_POSITION)  &&  jCamera[PRIMARY_KEY_CAMERA_POSITION].is_array ())
               for (int i = 0; i < 3  &&  i < static_cast<int> (jCamera[PRIMARY_KEY_CAMERA_POSITION].size ()); ++i)
                  Camera.aPosition[i] = jCamera[PRIMARY_KEY_CAMERA_POSITION][i].get<double> ();

            if (jCamera.contains (PRIMARY_KEY_CAMERA_ROTATION)  &&  jCamera[PRIMARY_KEY_CAMERA_ROTATION].is_array ())
               for (int i = 0; i < 4  &&  i < static_cast<int> (jCamera[PRIMARY_KEY_CAMERA_ROTATION].size ()); ++i)
                  Camera.aRotation[i] = jCamera[PRIMARY_KEY_CAMERA_ROTATION][i].get<double> ();

            m_pContext->Viewport ()->Camera (Camera);
         }

         if (jPrimary.contains (PRIMARY_KEY_BACKGROUND)  &&  jPrimary[PRIMARY_KEY_BACKGROUND].is_string ())
         {
            std::string  sHex   = jPrimary[PRIMARY_KEY_BACKGROUND].get<std::string> ();
            uint32_t     nColor = static_cast<uint32_t> (strtoul (sHex.c_str (), nullptr, 16));

            float fR = ((nColor >> 16) & 0xFF) / 255.0f;
            float fG = ((nColor >>  8) & 0xFF) / 255.0f;
            float fB = ( nColor        & 0xFF) / 255.0f;

            Background ({ fR, fG, fB, 1.0f });
         }

         if (jPrimary.contains (PRIMARY_KEY_AMBIENT)  &&  jPrimary[PRIMARY_KEY_AMBIENT].is_object ())
         {
            const nlohmann::json& jAmbient = jPrimary[PRIMARY_KEY_AMBIENT];

            Scene_Light_Ambient.fIntensity = jAmbient.value (PRIMARY_KEY_AMBIENT_BRIGHTNESS, SCENE_DEFAULT_BRIGHTNESS);
            HexColor (jAmbient.value (PRIMARY_KEY_AMBIENT_COLOR, std::string (SCENE_DEFAULT_LIGHT_COLOR)), Scene_Light_Ambient.rgbColor);

            bAmbient = true;
         }

         if (jPrimary.contains (PRIMARY_KEY_DIRECTIONAL)  &&  jPrimary[PRIMARY_KEY_DIRECTIONAL].is_object ())
         {
            const nlohmann::json& jDirectional = jPrimary[PRIMARY_KEY_DIRECTIONAL];

            Scene_Light_Directional.fIntensity = jDirectional.value (PRIMARY_KEY_DIRECTIONAL_BRIGHTNESS, SCENE_DEFAULT_BRIGHTNESS);
            HexColor (jDirectional.value (PRIMARY_KEY_DIRECTIONAL_COLOR, std::string (SCENE_DEFAULT_LIGHT_COLOR)), Scene_Light_Directional.rgbColor);

            // Aimed like a spot node: an authored quaternion rotates the identity
            // forward (+X) to give the direction the light travels. Absent a
            // rotation, the struct default (identity => +X) stands.
            if (jDirectional.contains (PRIMARY_KEY_DIRECTIONAL_ROTATION)  &&  jDirectional[PRIMARY_KEY_DIRECTIONAL_ROTATION].is_array ())
            {
               const nlohmann::json& jRotation = jDirectional[PRIMARY_KEY_DIRECTIONAL_ROTATION];
               if (jRotation.size () >= 4)
               {
                  QUAT qRotation = { jRotation[0].get<double> (), jRotation[1].get<double> (), jRotation[2].get<double> (), jRotation[3].get<double> () };
                  Scene_Light_Directional.vDirection = ForwardFromQuat (qRotation);
               }
            }

            bDirectional = true;
         }
      }

      if (!bAmbient  &&  !bDirectional)
         Scene_Light_Ambient.fIntensity = SCENE_DEFAULT_BRIGHTNESS;

      Ambient     (Scene_Light_Ambient);
      Directional (Scene_Light_Directional);
   }

   void OnMsfReady (NODE* pNode_Attach, FILE* pFile)
   {
      const std::string& sUrl = pFile->Url();

      FABRIC* pFabric;
      int nError = 0;

      std::vector<uint8_t> aData;

      pFile->ReadData (aData);

      if (!aData.empty ())
      {
         std::string sMsf (aData.begin (), aData.end ());

         MSF* pMsf = new MSF (m_pContext->Engine ());

         if (pMsf->Parse (sMsf, sUrl))
         {
            pMsf->VerifySignature ();
            pMsf->VerifyChain ();

            if ((pFabric = Fabric_Open (pNode_Attach, pMsf, sUrl)) != nullptr)
            {
               std::string sMsg = "Loaded MSF: " + pFabric->Container ()->Identity ()->DisplayName () + " (trust: " + std::to_string (pFabric->Container ()->Identity ()->eTrust) + ")";
               m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "SCENE", sMsg);
               m_pFabric_Root->Container ()->Stream ()->Info (sMsg, true);

               // Only the primary fabric drives page-wide presentation (initial camera pose, background colour).
               if (pNode_Attach == m_pNode_Primary)
                  Primary_Apply (pMsf);
            }
            else
            {
               std::string sErr = "Failed to open fabric " + sUrl;
               m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "SCENE", sErr);
               m_pFabric_Root->Container ()->Stream ()->Error (sErr, true);

               delete pMsf;

               nError = 404;
            }
         }
         else
         {
            std::string sErr = "Failed to parse MSF from " + sUrl;
            m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "SCENE", sErr);
            m_pFabric_Root->Container ()->Stream ()->Error (sErr, true);

            delete pMsf;

            nError = 404;
         }
      }
      else
      {
         std::string sErr = "MSF was empty for " + sUrl;
         m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "SCENE", sErr);
         m_pFabric_Root->Container ()->Stream ()->Error (sErr, true);

         nError = 404;
      }

      if (nError != 0)
         MsfError (pNode_Attach, pFile, nError);
   }

   void OnMsfFailed (NODE* pNode_Attach, FILE* pFile)
   {
      const std::string& sUrl = pFile->Url();

      std::string sErr = "Failed to fetch MSF from " + sUrl;
      m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "SCENE", sErr);
      m_pFabric_Root->Container ()->Stream ()->Error (sErr, true);

      MsfError (pNode_Attach, pFile, 404);
   }

   void MsfError (NODE* pNode_Attach, FILE* pFile, int nError)
   {
      // Only a failed PRIMARY load turns into an error page: a failed subsidiary
      // fabric leaves the rest of the scene intact and is reported to the console only.

      if (pNode_Attach == m_pNode_Primary)
      {
         const std::string& sUrl = pFile->Url ();

         // normally we would call the Host, but for now, just add a placeholder page to test
         FABRIC* pFabric_Error = Fabric_Open (m_pNode_Primary, nullptr, sUrl);

         if (pFabric_Error)
         {
            // make a json tree: one PANEL node at the origin, sized to a 16:9
            // aspect (Bound.Max carries only the quad's aspect). Head.Self is the
            // composed PANEL objectix with the "assign me an index" sentinel.
            nlohmann::json jBranch;
            jBranch[ERROR_KEY_HEAD][ERROR_KEY_HEAD_SELF] = OBJECTIX_COMPOSE (MAP_OBJECT::MAP_OBJECT_CLASS_PANEL, OBJECTIX_IDENTITY);
            jBranch[ERROR_KEY_NAME]                      = ERROR_PAGE_NAME;
            jBranch[ERROR_KEY_BOUND][ERROR_KEY_BOUND_MAX] = { ERROR_PAGE_ASPECT_W, ERROR_PAGE_ASPECT_H, 0.0 };

            uint64_t twPanelIx = pFabric_Error->Container ()->Branch_Add (pFabric_Error->FabricIx (), jBranch);

            // Branch_Add builds the panel with the engine's default document, so
            // point it at the constant error document instead.
            NODE*             pNode  = pFabric_Error->Container ()->Node_Find (twPanelIx);
            MAP_OBJECT_PANEL* pPanel = pNode ? dynamic_cast<MAP_OBJECT_PANEL*> (pNode->Map_Object ()) : nullptr;

            if (pPanel)
               pPanel->Source (ERROR_PAGE_DOCUMENT);

         //   Background ({ ERROR_PAGE_BG_R, ERROR_PAGE_BG_G, ERROR_PAGE_BG_B, 1.0f });
         }
      }
   }

   // -----------------------------------------------------------------------
// Internal Fabric management
// -----------------------------------------------------------------------

   FABRIC* Fabric_Open (NODE* pNode_Attach, MSF* pMsf, const std::string& sUrl)
   {
      FABRIC* pFabric = nullptr;

      CONTAINER* pContainer;
      uint64_t   twFabricIx;

      if ((pContainer = m_pContext->Container_Open (pMsf)) != nullptr)
      {
         {
            std::lock_guard<std::recursive_mutex> guard (m_mxScene);

            twFabricIx = ++m_twFabricIx_Next;

            pFabric = new FABRIC (m_pScene, pContainer, twFabricIx, pNode_Attach, pMsf);

            m_umpFabric[twFabricIx] = pFabric;
         }

         if (pFabric->Initialize (sUrl))
         {
            m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "SCENE", "Fabric Opened " + sUrl);
         }
         else pFabric = Fabric_Close (pFabric);
      }

      return pFabric;
   }

   FABRIC* Fabric_Close (FABRIC* pFabric)
   {
      MSF*       pMsf       = pFabric->Msf       ();
      CONTAINER* pContainer = pFabric->Container ();
      uint64_t   twFabricIx = pFabric->FabricIx  ();

      {
         std::lock_guard<std::recursive_mutex> guard (m_mxScene);

         delete pFabric;

         m_umpFabric.erase (twFabricIx);
      }

      m_pContext->Container_Close (pContainer);

      delete pMsf;

      return nullptr;
   }

   FABRIC* Fabric_Find (uint64_t twFabricIx) const
   {
      // A lock counter of sorts will probably be needed to make sure the fabric is not deleted while we're using it.

      std::lock_guard<std::recursive_mutex> guard (m_mxScene);

      FABRIC* pFabric = nullptr;

      auto it = m_umpFabric.find (twFabricIx);
      if (it != m_umpFabric.end ())
         pFabric = it->second;

      return pFabric;
   }

// -----------------------------------------------------------------------
// Backdrop (background colour)
//
// Set once and left until changed: Background() stores the value and trips a
// single changed-flag; the compositor test-and-clears it via Background_Consume()
// and pushes to the renderer only on change (including scene swaps), never
// every frame.
// -----------------------------------------------------------------------

   RGBA Background () const
   {
      return m_rgbaBackground;
   }

   void Background (const RGBA& rgbaBackground)
   {
      m_rgbaBackground = rgbaBackground;

      m_bBackdrop_Changed.store (true);
   }

   bool Background_Consume (RGBA& rgbaBackground)
   {
      bool bChanged = m_bBackdrop_Changed.exchange (false);

      if (bChanged)
         rgbaBackground = m_rgbaBackground;

      return bChanged;
   }

// -----------------------------------------------------------------------
// Scene-global lighting (ambient + primary directional)
//
// Authored once by the primary fabric and left until changed. Unlike the
// backdrop these are read fresh each frame by the compositor (the light list
// is rebuilt every frame), so no changed-flag is needed. Guarded by m_mxScene
// against the compositor reading mid-write.
// -----------------------------------------------------------------------

   void Ambient (const SCENE_LIGHT& Scene_Light)
   {
      std::lock_guard<std::recursive_mutex> Lock (m_mxScene);
      m_Scene_Light_Ambient = Scene_Light;
   }

   SCENE_LIGHT Ambient () const
   {
      std::lock_guard<std::recursive_mutex> Lock (m_mxScene);
      return m_Scene_Light_Ambient;
   }

   void Directional (const SCENE_LIGHT& Scene_Light)
   {
      std::lock_guard<std::recursive_mutex> Lock (m_mxScene);
      m_Scene_Light_Directional = Scene_Light;
   }

   SCENE_LIGHT Directional () const
   {
      std::lock_guard<std::recursive_mutex> Lock (m_mxScene);
      return m_Scene_Light_Directional;
   }

public:
   SCENE*                                m_pScene;
   CONTEXT*                              m_pContext;
   mutable std::recursive_mutex          m_mxScene;

   FABRIC*                               m_pFabric_Root;
   NODE*                                 m_pNode_Primary;

   uint64_t                              m_twFabricIx_Next;
   std::unordered_map<uint64_t, FABRIC*> m_umpFabric;

   RGBA                                  m_rgbaBackground;
   std::atomic<bool>                     m_bBackdrop_Changed;

   SCENE_LIGHT                           m_Scene_Light_Ambient;
   SCENE_LIGHT                           m_Scene_Light_Directional;
};


// ---------------------------------------------------------------------------
// SCENE
// ---------------------------------------------------------------------------

SCENE::SCENE (CONTEXT* pContext) :
   m_pImpl (new Impl (this, pContext))
{
}

bool SCENE::Initialize (const std::string& sUrl)
{
   return m_pImpl->Initialize (sUrl);
}

SCENE::~SCENE ()
{
   delete m_pImpl;
   m_pImpl = nullptr;
}

// -----------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------

SNEEZE::ENGINE*  SCENE::Engine         () const { return m_pImpl->m_pContext->Engine (); }
SNEEZE::CONTEXT* SCENE::Context        () const { return m_pImpl->m_pContext; }
SNEEZE::NETWORK* SCENE::Network        () const { return m_pImpl->m_pContext->Network (); }
FABRIC*          SCENE::Fabric_Root    () const { return m_pImpl->m_pFabric_Root; }
FABRIC*          SCENE::Fabric_Primary () const { return m_pImpl->m_pNode_Primary ? m_pImpl->m_pNode_Primary->Fabric_Attachment () : nullptr; }

// -----------------------------------------------------------------------
// Internal functions
// -----------------------------------------------------------------------

void    SCENE::OnMsfReady   (NODE* pNode_Attach, SNEEZE::FILE* pFile)     {        m_pImpl->OnMsfReady   (pNode_Attach, pFile); }
void    SCENE::OnMsfFailed  (NODE* pNode_Attach, SNEEZE::FILE* pFile)     {        m_pImpl->OnMsfFailed  (pNode_Attach, pFile); }

// -----------------------------------------------------------------------
// Scene Internal functions
// -----------------------------------------------------------------------

RGBA     SCENE::Background         () const                                              { return m_pImpl->Background (); }
void     SCENE::Background         (const RGBA& rgbaBackground)                          {        m_pImpl->Background (rgbaBackground); }
bool     SCENE::Background_Consume (RGBA& rgbaBackground)                                { return m_pImpl->Background_Consume (rgbaBackground); }

void        SCENE::Ambient         (const SCENE_LIGHT& Light)                            {        m_pImpl->Ambient (Light); }
void        SCENE::Directional     (const SCENE_LIGHT& Light)                            {        m_pImpl->Directional (Light); }
SCENE_LIGHT SCENE::Ambient         () const                                              { return m_pImpl->Ambient (); }
SCENE_LIGHT SCENE::Directional     () const                                              { return m_pImpl->Directional (); }
void        SCENE::Fabric_Spawn    (NODE* pNode_Attach, const std::string& sUrl)         {        m_pImpl->Fabric_Spawn (pNode_Attach, sUrl); }
FABRIC*     SCENE::Fabric_Close    (FABRIC* pFabric)                                     { return m_pImpl->Fabric_Close (pFabric); }
FABRIC*     SCENE::Fabric_Find     (uint64_t twFabricIx)                           const { return m_pImpl->Fabric_Find  (twFabricIx); }
