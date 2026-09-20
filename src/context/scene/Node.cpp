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

#include "context/viewport/Viewport.h"
#include "Context.h"
#include "stb/stb_image.h"
#include "ui/Ui_Panel.h"

#include <chrono>

using namespace SNEEZE;

using CONTAINER = SNEEZE::CONTAINER;

// A fetched resource is identified by its content, not its URL: a binary GLB
// begins with the ASCII magic "glTF", and a glTF JSON document begins (after any
// leading whitespace) with '{'. Image textures are binary and match neither, so
// anything else is decoded as a texture.
static bool IsGltf (const std::vector<uint8_t>& aData)
{
   bool bGltf = false;

   if (aData.size () >= 4  &&  aData[0] == 'g'  &&  aData[1] == 'l'  &&  aData[2] == 'T'  &&  aData[3] == 'F')
      bGltf = true;
   else
   {
      size_t nFirst = 0;
      while (nFirst < aData.size ()  &&  std::isspace (static_cast<unsigned char> (aData[nFirst])))
         nFirst++;

      bGltf = (nFirst < aData.size ()  &&  aData[nFirst] == '{');
   }

   return bGltf;
}

static MAT4 Mat4_Identity ()
{
   MAT4 m = { { 1.0, 0.0, 0.0, 0.0,  0.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, 0.0, 0.0, 1.0, } };
   return m;
}

static const std::string sResource_Supplementary_None;

// ---------------------------------------------------------------------------
// SEQLOCK
// ---------------------------------------------------------------------------

class SEQLOCK
{
public:
   SEQLOCK ();

   uint32_t BeginRead () const;
   bool     EndRead (uint32_t nSeq) const;
   void     BeginWrite ();
   void     EndWrite ();

private:
   std::atomic<uint32_t> m_nSequence;
};

SEQLOCK::SEQLOCK () : m_nSequence (0) {}

uint32_t SEQLOCK::BeginRead () const
{
   uint32_t nSeq;
   do
   {
      nSeq = m_nSequence.load (std::memory_order_acquire);
   }
   while (nSeq & 1);
   return nSeq;
}

bool SEQLOCK::EndRead (uint32_t nSeq) const
{
   std::atomic_thread_fence (std::memory_order_acquire);
   return m_nSequence.load (std::memory_order_relaxed) == nSeq;
}

void SEQLOCK::BeginWrite ()
{
   uint32_t nExpected = m_nSequence.load (std::memory_order_relaxed);
   uint32_t nDesired;
   do
   {
      while (nExpected & 1)
         nExpected = m_nSequence.load (std::memory_order_relaxed);
      nDesired = nExpected + 1;
   }
   while (!m_nSequence.compare_exchange_weak (nExpected, nDesired,
      std::memory_order_acquire, std::memory_order_relaxed));
}

void SEQLOCK::EndWrite ()
{
   m_nSequence.fetch_add (1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// NODE::Impl
// ---------------------------------------------------------------------------

class NODE::Impl : public IFILE
{
public:
   Impl (NODE* pNode, FABRIC* pFabric, NODE* pNode_Parent, RMAP::MAP::MAP_OBJECT* pMap_Object) :
      m_pNode              (pNode),
      m_pFabric            (pFabric),
      m_pNode_Parent       (pNode_Parent),
      m_pMap_Object        (pMap_Object),
      m_pFabric_Attachment (nullptr),
      m_pFile              (nullptr),
      m_bPrivate           (false),
      m_pRenderModel       (nullptr),
      m_bRenderModelReady  (false),
      m_dTime_Anim         (0.0),
      m_nClip_Anim         (0),
      m_bAnim_Clock        (false),
      m_bVrma_Fetch        (false),
      m_bAnim_Vrma         (false),
      m_pPanel             (nullptr)
   {
      if (m_pNode_Parent)
         m_pNode_Parent->Node_Add (m_pNode);
      else m_pFabric->Node_Root (m_pNode);
   }

   bool Initialize ()
   {
      bool bResult;
      RMAP::MAP::MAP_OBJECT_POD Pod;

      if (m_pMap_Object)
      {
         bResult = true;

         m_pMap_Object->GetPOD (Pod);

         if (Pod.Resource.sReference[0] != '\0')
         {
            if (Pod.Type.bSubtype == 255)
            {
               std::string sUrl = m_pFabric->Resolve (Pod.Resource.sReference);

               if (!sUrl.empty ())
               {
                  m_pFabric->Scene ()->Fabric_Spawn (m_pNode, sUrl);
               }
            }
            else
               Resource_Request ();
         }
         else if (m_pMap_Object->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_PANEL)
         {
            m_pPanel = new DEP::UI_PANEL ();
         }
      }
      else bResult = false;

      return bResult;
   }

   ~Impl ()
   {
      while (!m_apNode.empty ())
         m_pFabric->Container ()->Node_Close (m_apNode.back ()->ObjectIx ());

      if (m_pFabric_Attachment)
      {
         m_pFabric->Scene()->Fabric_Close(m_pFabric_Attachment);
         m_pFabric_Attachment = nullptr;
      }

      Resource_Release ();

      if (m_pNode_Parent)
         m_pNode_Parent->Node_Remove (m_pNode);
      else m_pFabric->Node_Root (nullptr);

      Gltf_Render_Model_Release (m_pRenderModel);
      m_pRenderModel = nullptr;
      delete m_pPanel;
   }

// -----------------------------------------------------------------------
// Resource management (fetch by URL, dispatch by content)
// -----------------------------------------------------------------------

   void Resource_Request ()
   {
      RMAP::MAP::MAP_OBJECT_POD Pod;

      m_pMap_Object->GetPOD (Pod);
      if (m_pMap_Object  && Pod.Resource.sReference[0] != '\0')
if (strncmp (Pod.Resource.sReference, "action:", 7) != 0) // TODO: REMOVE THIS TEMPORARY!!!
         m_pFile = m_pFabric->Container ()->Cache ()->File_Open (m_pFabric->Resolve (Pod.Resource.sReference), this);
   }

   void Resource_Release ()
   {
      if (m_pFile)
      {
         m_pFile->Close ();
         m_pFile = nullptr;
      }
   }

   // A fetched resource is sniffed by content: a glTF model (binary GLB or glTF
   // JSON) becomes this node's render model; anything else is decoded as an
   // image texture on the map object.
   void Resource_Load (const std::vector<uint8_t>& aData, const std::string& sUrl)
   {
      if (IsGltf (aData))
         Gltf_Load (aData, sUrl);
      else Texture_Load (aData);
   }

   void Gltf_Load (const std::vector<uint8_t>& aData, const std::string& sUrl)
   {
      GLTF_RENDER_MODEL* pModel = nullptr;

      if (Gltf_Render_Model_Acquire (sUrl, pModel))
      {
         Gltf_Render_Model (pModel);
         Vrma_Request ();
      }
      else
      {
         DEP::GLTF_MODEL model;
         std::string     sError;

         if (DEP::GLTF::Load (aData.data (), aData.size (), model, sError))
         {
            // Built in place -- MESH_DATA borrows into the model's own storage
            // -- then published into the process-wide URL cache and stored on
            // this node for the compositor.
            pModel = new GLTF_RENDER_MODEL ();

            if (Gltf_Render_Model_Build (std::move (model), Mat4_Identity (), *pModel))
            {
               Gltf_Render_Model_Publish (pModel, sUrl, pModel);
               Gltf_Render_Model (pModel);

               if (m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Engine ())
               {
                  uint32_t nVertex = 0;
                  uint32_t nTextured = 0;
                  for (const MESH_DATA& mesh : pModel->aMesh)
                  {
                     nVertex += mesh.uCount_Vertex;
                     if (mesh.pbTexturePixels  &&  mesh.dimTexture.nW > 0  &&  mesh.dimTexture.nH > 0)
                     {
                        nTextured++;
                        if (nTextured == 1)
                        {
                           m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "GLTF",
                              "PBR draw metallic=" + std::to_string (mesh.fMetallic)
                              + " roughness=" + std::to_string (mesh.fRoughness)
                              + " albedo=" + std::to_string (mesh.dimTexture.nW) + "x" + std::to_string (mesh.dimTexture.nH)
                              + " " + sUrl);
                        }
                     }
                  }
                  m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "GLTF",
                     "loaded " + std::to_string (pModel->aMesh.size ()) + " draws, "
                     + std::to_string (nTextured) + " textured, "
                     + std::to_string (nVertex) + " vertices (" + std::to_string (aData.size ()) + " bytes) " + sUrl);
               }

               Vrma_Request ();
            }
            else
            {
               delete pModel;
               if (m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Engine ())
                  m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "GLTF",
                     "glTF produced no drawable primitives (" + std::to_string (aData.size ()) + " bytes) " + sUrl);
            }
         }
         else if (m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Engine ())
         {
            m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "GLTF",
               "glTF load failed (" + std::to_string (aData.size ()) + " bytes): " + sError + " " + sUrl);
         }
      }
   }

   void Texture_Load (const std::vector<uint8_t>& aData)
   {
      int nW = 0, nH = 0, nChannels = 0;
      unsigned char* pPixels = stbi_load_from_memory (aData.data (), static_cast<int> (aData.size ()), &nW, &nH, &nChannels, 4);

      if (pPixels)
      {
         m_pMap_Object->SetTexture (pPixels, nW, nH);

         stbi_image_free (pPixels);
      }
   }

   void Vrma_Request ()
   {
      const std::string& sVrma = Resource_Supplementary ("vrma");
      if (!sVrma.empty ()  &&  m_pFabric  &&  m_pFabric->Container ()  &&  m_pFabric->Container ()->Cache ())
      {
         m_bVrma_Fetch = true;
         m_pFile = m_pFabric->Container ()->Cache ()->File_Open (m_pFabric->Resolve (sVrma), this);
         if (!m_pFile)
            m_bVrma_Fetch = false;
      }
   }

   void Vrma_Load (const std::vector<uint8_t>& aData, const std::string& sUrl)
   {
      const GLTF_RENDER_MODEL* pModel = Gltf_Render_Model ();
      if (pModel)
      {
         DEP::GLTF_MODEL modelVrma;
         std::string     sError;

         if (DEP::GLTF::Load (aData.data (), aData.size (), modelVrma, sError))
         {
            DEP::GLTF_ANIMATION anim;
            if (Gltf_Vrma_Retarget (modelVrma, pModel->model, anim))
            {
               m_Anim_Vrma = std::move (anim);
               m_dTime_Anim  = 0.0;
               m_bAnim_Clock = false;
               m_bAnim_Vrma.store (true, std::memory_order_release);
               if (m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Engine ())
                  m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "GLTF",
                     "VRMA retargeted " + std::to_string (m_Anim_Vrma.aChannel.size ()) + " channels (" + std::to_string (aData.size ()) + " bytes) " + sUrl);
            }
            else if (m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Engine ())
               m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "GLTF",
                  "VRMA retarget produced no humanoid channels " + sUrl);
         }
         else if (m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Engine ())
            m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "GLTF",
               "VRMA load failed (" + std::to_string (aData.size ()) + " bytes): " + sError + " " + sUrl);
      }
   }

   void OnFileReady (FILE* pFile) override
   {
      std::vector<uint8_t> aData;
      std::string          sUrl;
      const bool           bVrma = m_bVrma_Fetch;

      // Load the resource BEFORE relinquishing the file handle. m_pFile is the
      // barrier a concurrent teardown uses to synchronize -- while it is
      // non-null and the fetch-completion lock is held, ~Impl blocks in
      // Resource_Release (FILE::Close) until this callback returns. Nulling it
      // before the load would let the node be freed out from under
      // Resource_Load / Vrma_Load (use-after-free).
      if (m_pMap_Object)
      {
         pFile->ReadData (aData);
         sUrl = pFile->Url ();

         if (!aData.empty ())
         {
            if (bVrma)
               Vrma_Load (aData, sUrl);
            else
               Resource_Load (aData, sUrl);
         }
      }

      pFile->Close ();
      m_pFile = nullptr;
      m_bVrma_Fetch = false;
   }

   void OnFileFailed (FILE* pFile) override
   {
      const bool bVrma = m_bVrma_Fetch;
      pFile->Close ();
      m_pFile = nullptr;
      m_bVrma_Fetch = false;
      if (bVrma  &&  m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Engine ())
         m_pFabric->Scene ()->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "GLTF", "VRMA fetch failed");
   }

// -----------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------

   NODE* Parent () const
   {
      NODE* pResult = m_pNode_Parent;

      if (!pResult  &&  m_pFabric)
         pResult = m_pFabric->Node_Attach ();

      return pResult;
   }

   NODE* Child (int nPosition) const
   {
      std::lock_guard<std::mutex> guard (m_mutex_pNode);

      NODE* pResult = nullptr;
      if (nPosition >= 0  &&  nPosition < static_cast<int> (m_apNode.size ()))
         pResult = m_apNode[nPosition];

      return pResult;
   }

   int Node_Count () const
   {
      std::lock_guard<std::mutex> guard (m_mutex_pNode);
      return static_cast<int> (m_apNode.size ());
   }


// -----------------------------------------------------------------------
// Called internally from child nodes
// -----------------------------------------------------------------------

   void Node_Add (NODE* pNode_Child)
   {
      std::lock_guard<std::mutex> guard (m_mutex_pNode);

      m_apNode.push_back (pNode_Child);
   }

   void Node_Remove (NODE* pNode_Child)
   {
      if (pNode_Child)
      {
         std::lock_guard<std::mutex> guard (m_mutex_pNode);

         auto it = std::find (m_apNode.begin (), m_apNode.end (), pNode_Child);
         if (it != m_apNode.end ())
         {
            *it = m_apNode.back ();
            m_apNode.pop_back ();
         }
      }
   }

   // glTF/GLB model: built on the network thread, published via
   // m_bRenderModelReady, and read on the compositor thread. Cached models are
   // refcounted by URL (Acquire/Release); the pointer is immutable once
   // published, so the acquire/release pair alone makes it safe to read
   // without a lock.
   const GLTF_RENDER_MODEL* Gltf_Render_Model () const
   {
      const GLTF_RENDER_MODEL* pResult = nullptr;

      if (m_bRenderModelReady.load (std::memory_order_acquire))
         pResult = m_pRenderModel;

      return pResult;
   }

   void Gltf_Render_Model (GLTF_RENDER_MODEL* pModel)
   {
      if (m_pRenderModel != pModel)
      {
         Gltf_Render_Model_Release (m_pRenderModel);
         m_pRenderModel = pModel;
         m_aBonePalette.clear ();
         m_aNode_Pose.clear ();
         m_aMeshWorld.clear ();
         if (pModel)
            m_aBonePalette = pModel->aBonePalette;
         m_dTime_Anim  = 0.0;
         m_nClip_Anim  = 0;
         m_bAnim_Clock = false;
         m_Anim_Vrma   = DEP::GLTF_ANIMATION ();
         m_bAnim_Vrma.store (false, std::memory_order_release);
         m_bRenderModelReady.store (pModel != nullptr, std::memory_order_release);
         if (pModel  &&  m_pFabric  &&  m_pFabric->Scene ()  &&  m_pFabric->Scene ()->Context ()  &&  m_pFabric->Scene ()->Context ()->Viewport ())
            m_pFabric->Scene ()->Context ()->Viewport ()->Mesh_Notify ();
      }
   }

   const std::string& Resource_Supplementary (const std::string& sKey) const
   {
      const std::string* psResult = &sResource_Supplementary_None;

      auto it = m_umpResource_Supplementary.find (sKey);
      if (it != m_umpResource_Supplementary.end ())
         psResult = &it->second;

      return *psResult;
   }

   void Resource_Supplementary (const std::string& sKey, const std::string& sReference)
   {
      if (!sKey.empty ())
      {
         if (sReference.empty ())
            m_umpResource_Supplementary.erase (sKey);
         else
            m_umpResource_Supplementary[sKey] = sReference;
      }
   }

   const float* BonePalette (uint32_t nSkin, uint32_t& nBone) const
   {
      const float* pfMatrix = nullptr;
      nBone = 0;

      if (nSkin < m_aBonePalette.size ()  &&  !m_aBonePalette[nSkin].empty ())
      {
         pfMatrix = m_aBonePalette[nSkin].data ();
         nBone    = static_cast<uint32_t> (m_aBonePalette[nSkin].size () / 16);
      }

      return pfMatrix;
   }

   bool MeshWorld (uint32_t nDrawIx, MAT4& mWorld) const
   {
      bool bOk = false;

      if (nDrawIx < m_aMeshWorld.size ())
      {
         const MAT4F& mSrc = m_aMeshWorld[nDrawIx];
         for (int n = 0; n < 16; n++)
            mWorld.d[n] = mSrc.f[n];
         bOk = true;
      }

      return bOk;
   }

   void BonePalette (uint32_t nSkin, const float* pfMatrix, uint32_t nBone)
   {
      if (nSkin >= m_aBonePalette.size ())
         m_aBonePalette.resize (static_cast<size_t> (nSkin) + 1);

      uint32_t nCount = nBone;
      if (nCount > 255)
         nCount = 255;

      if (pfMatrix  &&  nCount > 0)
         m_aBonePalette[nSkin].assign (pfMatrix, pfMatrix + static_cast<size_t> (nCount) * 16);
      else
         m_aBonePalette[nSkin].clear ();
   }

   void Animation_Tick ()
   {
      const GLTF_RENDER_MODEL* pModel = Gltf_Render_Model ();
      if (pModel)
      {
         const DEP::GLTF_MODEL& model = pModel->model;
         const DEP::GLTF_ANIMATION* pAnim = nullptr;
         if (m_bAnim_Vrma.load (std::memory_order_acquire))
            pAnim = &m_Anim_Vrma;
         else if (m_nClip_Anim < model.aAnimation.size ())
            pAnim = &model.aAnimation[m_nClip_Anim];

         if (pAnim  &&  (!model.aSkin.empty ()  ||  !pAnim->aChannel.empty ()))
         {
            const double dDuration = pAnim->dDuration;
            if (dDuration > 0.0)
            {
               const std::chrono::steady_clock::time_point tpNow = std::chrono::steady_clock::now ();
               double dDt = 0.0;
               if (m_bAnim_Clock)
               {
                  dDt = std::chrono::duration<double> (tpNow - m_tpAnim).count ();
                  if (dDt < 0.0)
                     dDt = 0.0;
                  if (dDt > 0.25)
                     dDt = 0.25;
               }
               m_tpAnim      = tpNow;
               m_bAnim_Clock = true;

               m_dTime_Anim += dDt;
               while (m_dTime_Anim >= dDuration)
                  m_dTime_Anim -= dDuration;

               Gltf_Render_Model_Pose (*pModel, *pAnim, m_dTime_Anim, m_aNode_Pose, m_aBonePalette, &m_aMeshWorld);
            }
         }
      }
   }

   void Source (const std::string& sSource)
   {
      m_pPanel->Source (sSource);
   }

   bool Render (ENGINE* pEngine, int nWidth, int nHeight)
   {
      return m_pPanel->Render (pEngine, nWidth, nHeight);
   }

   const uint8_t* Pixels () const
   {
      return m_pPanel->Pixels ();
   }

   int Width () const
   {
      return m_pPanel->Width ();
   }

   int Height () const
   {
      return m_pPanel->Height ();
   }

   uint32_t Serial () const
   {
      return m_pPanel->Serial ();
   }

public:
   FABRIC*                             m_pFabric;
   NODE*                               m_pNode;
   NODE*                               m_pNode_Parent;
   RMAP::MAP::MAP_OBJECT*              m_pMap_Object;
   FABRIC*                             m_pFabric_Attachment;
   FILE*                               m_pFile;
   SEQLOCK                             m_Seqlock;

   bool                                m_bPrivate;

   std::vector<NODE*>                  m_apNode;
   mutable std::mutex                  m_mutex_pNode;

   GLTF_RENDER_MODEL*                  m_pRenderModel;
   std::atomic<bool>                   m_bRenderModelReady;
   std::vector<std::vector<float>>     m_aBonePalette;
   std::vector<DEP::GLTF_NODE>         m_aNode_Pose;
   std::vector<MAT4F>                  m_aMeshWorld;
   std::unordered_map<std::string, std::string> m_umpResource_Supplementary;
   double                              m_dTime_Anim;
   uint32_t                            m_nClip_Anim;
   std::chrono::steady_clock::time_point m_tpAnim;
   bool                                m_bAnim_Clock;
   bool                                m_bVrma_Fetch;
   DEP::GLTF_ANIMATION                 m_Anim_Vrma;
   std::atomic<bool>                   m_bAnim_Vrma;

   DEP::UI_PANEL*                      m_pPanel;
};

// ---------------------------------------------------------------------------
// NODE
// ---------------------------------------------------------------------------

NODE::NODE (FABRIC* pFabric, NODE* pNode_Parent, RMAP::MAP::MAP_OBJECT* pMap_Object) :
   m_pImpl (new Impl (this, pFabric, pNode_Parent, pMap_Object))
{
}

bool NODE::Initialize ()
{
   return m_pImpl->Initialize ();
}

NODE::~NODE ()
{
   delete m_pImpl;
   m_pImpl = nullptr;
}

// -----------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------

uint64_t    NODE::ObjectIx          ()                    const { return m_pImpl->m_pMap_Object->m_twObjectIx; }

std::string NODE::Name () const
{
   std::string sResult;

   if (m_pImpl->m_pMap_Object)
   {
      RMAP::MAP::MAP_OBJECT_POD Pod;

      m_pImpl->m_pMap_Object->GetPOD (Pod);

      // m_Name.wsName is a fixed-size UTF-16 buffer (BMP only for names).
      const uint16_t* pwName = Pod.Name.wsName;
      const int       nMax   = static_cast<int> (sizeof (Pod.Name.wsName) / sizeof (uint16_t));

      for (int i = 0; i < nMax  &&  pwName[i] != 0; i++)
      {
         uint32_t cp = pwName[i];

         if (cp < 0x80)
         {
            sResult += static_cast<char> (cp);
         }
         else if (cp < 0x800)
         {
            sResult += static_cast<char> (0xC0 | (cp >> 6));
            sResult += static_cast<char> (0x80 | (cp & 0x3F));
         }
         else
         {
            sResult += static_cast<char> (0xE0 | (cp >> 12));
            sResult += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
            sResult += static_cast<char> (0x80 | (cp & 0x3F));
         }
      }
   }

   return sResult;
}

std::string NODE::ClassName () const
{
   return m_pImpl->m_pMap_Object ? RMAP::MAP::MAP_OBJECT::ClassName (static_cast <RMAP::MAP::MAP_OBJECT_CLASS> (m_pImpl->m_pMap_Object->m_wClass)) : "";
}

std::string NODE::TypeName () const
{
   std::string sResult;

   if (m_pImpl->m_pMap_Object)
   {
      RMAP::MAP::MAP_OBJECT_POD Pod;

      m_pImpl->m_pMap_Object->GetPOD (Pod);

      uint8_t bType = Pod.Type.bType;

      // Type identifiers are class-specific; only celestial bodies have named
      // types today. Other classes fall back to the raw numeric type.
      if (m_pImpl->m_pMap_Object->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL)
         sResult = RMAP::MAP::MAP_OBJECT_CELESTIAL::GetTypeName (static_cast<RMAP::MAP::MAP_OBJECT_CELESTIAL::eTYPE> (bType));

      if (sResult.empty ())
         sResult = "type" + std::to_string (static_cast<int> (bType));
   }

   return sResult;
}

int NODE::Subtype () const
{
   RMAP::MAP::MAP_OBJECT_POD Pod;

   m_pImpl->m_pMap_Object->GetPOD (Pod);

   return m_pImpl->m_pMap_Object ? static_cast<int> (Pod.Type.bSubtype) : 0;
}

FABRIC*     NODE::Fabric            ()                    const { return m_pImpl->m_pFabric; }
FABRIC*     NODE::Fabric_Attachment ()                    const { return m_pImpl->m_pFabric_Attachment; }

bool                   NODE::IsPrivate  ()                const { return m_pImpl->m_bPrivate; }
RMAP::MAP::MAP_OBJECT* NODE::Map_Object ()                const { return m_pImpl->m_pMap_Object; }

NODE*       NODE::Parent            ()                    const { return m_pImpl->Parent (); }
NODE*       NODE::Child             (int nPosition)       const { return m_pImpl->Child (nPosition); }
int         NODE::Node_Count        ()                    const { return m_pImpl->Node_Count (); }

// -----------------------------------------------------------------------
// Mutators
// -----------------------------------------------------------------------

void        NODE::Private           (bool bPrivate)             {        m_pImpl->m_bPrivate   = bPrivate; }

// -----------------------------------------------------------------------
// Called internally from child nodes
// -----------------------------------------------------------------------

void        NODE::Fabric_Add        (FABRIC* pFabric_Child)
{
   m_pImpl->m_pFabric_Attachment = pFabric_Child;
   m_pImpl->m_pFabric->Fabric_Add (pFabric_Child);
}

void        NODE::Fabric_Remove     (FABRIC* pFabric_Child)
{
   m_pImpl->m_pFabric_Attachment = nullptr;
   m_pImpl->m_pFabric->Fabric_Remove (pFabric_Child);
}

void        NODE::Node_Add          (NODE* pNode_Child)         {        m_pImpl->Node_Add    (pNode_Child); }
void        NODE::Node_Remove       (NODE* pNode_Child)         {        m_pImpl->Node_Remove (pNode_Child); }

const GLTF_RENDER_MODEL* NODE::Gltf_Render_Model () const                     { return m_pImpl->Gltf_Render_Model (); }
void                     NODE::Gltf_Render_Model (GLTF_RENDER_MODEL* pModel)  { m_pImpl->Gltf_Render_Model (pModel);  }

const std::string& NODE::Resource_Supplementary (const std::string& sKey) const
{
   return m_pImpl->Resource_Supplementary (sKey);
}

void NODE::Resource_Supplementary (const std::string& sKey, const std::string& sReference)
{
   m_pImpl->Resource_Supplementary (sKey, sReference);
}

const float* NODE::BonePalette (uint32_t nSkin, uint32_t& nBone) const
{
   return m_pImpl->BonePalette (nSkin, nBone);
}

void NODE::BonePalette (uint32_t nSkin, const float* pfMatrix, uint32_t nBone)
{
   m_pImpl->BonePalette (nSkin, pfMatrix, nBone);
}

bool NODE::MeshWorld (uint32_t nDrawIx, MAT4& mWorld) const
{
   return m_pImpl->MeshWorld (nDrawIx, mWorld);
}

void NODE::Animation_Tick ()
{
   m_pImpl->Animation_Tick ();
}

void NODE::Source (const std::string& sSource)
{
   m_pImpl->Source (sSource);
}

bool NODE::Render (ENGINE* pEngine, int nWidth, int nHeight)
{
   return m_pImpl->Render (pEngine, nWidth, nHeight);
}

const uint8_t* NODE::Pixels () const
{
   return m_pImpl->Pixels ();
}

int NODE::Width () const
{
   return m_pImpl->Width ();
}

int NODE::Height () const
{
   return m_pImpl->Height ();
}

uint32_t NODE::Serial () const
{
   return m_pImpl->Serial ();
}
