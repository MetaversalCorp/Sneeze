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

#include "Map_Object.h"
#include "context/viewport/Viewport.h"
#include "ui/Ui_Panel.h"

using namespace SNEEZE;

// Pull the next Unicode code point out of a UTF-8 string, advancing i.
// Malformed or truncated sequences yield U+FFFD and advance a single byte, so
// the decoder always makes progress and never reads past the string.
static uint32_t Utf8_Decode (const std::string& sText, size_t& i)
{
   size_t   nSize = sText.size ();
   uint32_t cp    = 0xFFFD;
   uint8_t  c0    = static_cast<uint8_t> (sText[i]);

   if (c0 < 0x80)
   {
      cp = c0;
      i += 1;
   }
   else if ((c0 & 0xE0) == 0xC0  &&  i + 1 < nSize  &&  (static_cast<uint8_t> (sText[i + 1]) & 0xC0) == 0x80)
   {
      cp = ((c0 & 0x1F) << 6) | (static_cast<uint8_t> (sText[i + 1]) & 0x3F);
      i += 2;
   }
   else if ((c0 & 0xF0) == 0xE0  &&  i + 2 < nSize  &&  (static_cast<uint8_t> (sText[i + 1]) & 0xC0) == 0x80  &&  (static_cast<uint8_t> (sText[i + 2]) & 0xC0) == 0x80)
   {
      cp = ((c0 & 0x0F) << 12) | ((static_cast<uint8_t> (sText[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t> (sText[i + 2]) & 0x3F);
      i += 3;
   }
   else if ((c0 & 0xF8) == 0xF0  &&  i + 3 < nSize  &&  (static_cast<uint8_t> (sText[i + 1]) & 0xC0) == 0x80  &&  (static_cast<uint8_t> (sText[i + 2]) & 0xC0) == 0x80  &&  (static_cast<uint8_t> (sText[i + 3]) & 0xC0) == 0x80)
   {
      cp = ((c0 & 0x07) << 18) | ((static_cast<uint8_t> (sText[i + 1]) & 0x3F) << 12) | ((static_cast<uint8_t> (sText[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t> (sText[i + 3]) & 0x3F);
      i += 4;
   }
   else
   {
      i += 1;
   }

   return cp;
}

void SNEEZE::Name_Set (MAP_OBJECT::MAP_OBJECT_NAME& name, const std::string& sName)
{
   memset (&name, 0, sizeof (name));

   size_t i      = 0;
   size_t nCount = 0;

   while (i < sName.size ()  &&  nCount < 47)
   {
      uint32_t cp = Utf8_Decode (sName, i);

      if (cp > 0xFFFF)
         cp = 0xFFFD;

      name.wsName[nCount++] = static_cast<uint16_t> (cp);
   }
}

static QUAT QuatMultiply (const QUAT& q1, const QUAT& q2)
{
   QUAT r;

   r.dX = q1.dW*q2.dX + q1.dX*q2.dW + q1.dY*q2.dZ - q1.dZ*q2.dY;
   r.dY = q1.dW*q2.dY - q1.dX*q2.dZ + q1.dY*q2.dW + q1.dZ*q2.dX;
   r.dZ = q1.dW*q2.dZ + q1.dX*q2.dY - q1.dY*q2.dX + q1.dZ*q2.dW;
   r.dW = q1.dW*q2.dW - q1.dX*q2.dX - q1.dY*q2.dY - q1.dZ*q2.dZ;

   return r;
}

static VEC3 RotateByQuat (double qx, double qy, double qz, double qw, double vx, double vy, double vz)
{
   double cx1 = qy * vz - qz * vy;
   double cy1 = qz * vx - qx * vz;
   double cz1 = qx * vy - qy * vx;
   double cx2 = qy * cz1 - qz * cy1;
   double cy2 = qz * cx1 - qx * cz1;
   double cz2 = qx * cy1 - qy * cx1;

   return {
      vx + 2.0 * (qw * cx1 + cx2),
      vy + 2.0 * (qw * cy1 + cy2),
      vz + 2.0 * (qw * cz1 + cz2),
   };
}

static double SolveKepler (double dM_rad, double dEcc)
{
   double dE = dEcc > 0.8 ? PI : dM_rad;

   for (int i = 0; i < 50; i++)
   {
      double dDelta = dE - dEcc * std::sin (dE) - dM_rad;
      if (std::abs (dDelta) < 1e-15) break;
      dE -= dDelta / (1.0 - dEcc * std::cos (dE));
   }

   return dE;
}

// ---------------------------------------------------------------------------
// MAP_OBJECT::Impl
// ---------------------------------------------------------------------------

class MAP_OBJECT::Impl
{
public:
   Impl () :
      m_nTextureWidth (0),
      m_nTextureHeight (0),
      m_nTextureChannels (0),
      m_bTextureReady (false),
      m_pRenderModel (nullptr),
      m_bRenderModelReady (false)
   {
   }

   ~Impl ()
   {
      delete m_pRenderModel;
   }

   bool GetTexture (const uint8_t*& pTex, int& nTexW, int& nTexH)
   {
      bool bResult = false;

      if (m_bTextureReady.load ())
      {
         m_CS.lock ();
         {
            bResult = true;

            pTex = m_aTexturePixels.data ();
            nTexW = m_nTextureWidth;
            nTexH = m_nTextureHeight;
         }
         m_CS.unlock ();
      }

      return bResult;
   }

   void SetTexture (const uint8_t* pTex, int nTexW, int nTexH)
   {
      m_CS.lock ();
      {
         m_aTexturePixels.assign (pTex, pTex + nTexW * nTexH * 4);
         m_nTextureWidth      = nTexW;
         m_nTextureHeight     = nTexH;
         m_nTextureChannels   = 4;
      }
      m_CS.unlock ();

      m_bTextureReady.store (true);
   }

   // glTF/GLB model: built on the network thread, published write-once via
   // m_bRenderModelReady, and read on the compositor thread. The model is
   // immutable once published (its MESH_DATA borrows into its own storage), so
   // the acquire/release pair alone makes it safe to read without a lock.
   const GLTF_RENDER_MODEL* Gltf_Render_Model () const
   {
      const GLTF_RENDER_MODEL* pResult = nullptr;

      if (m_bRenderModelReady.load (std::memory_order_acquire))
         pResult = m_pRenderModel;

      return pResult;
   }

   void Gltf_Render_Model (GLTF_RENDER_MODEL* pModel)
   {
      m_pRenderModel = pModel;

      m_bRenderModelReady.store (true, std::memory_order_release);
   }

private:
   mutable std::mutex            m_CS;
   std::vector<uint8_t>          m_aTexturePixels;
   int                           m_nTextureWidth;
   int                           m_nTextureHeight;
   int                           m_nTextureChannels;
   std::atomic<bool>             m_bTextureReady;

   GLTF_RENDER_MODEL*            m_pRenderModel;
   std::atomic<bool>             m_bRenderModelReady;
};

// ---------------------------------------------------------------------------
// MAP_OBJECT
// ---------------------------------------------------------------------------

MAP_OBJECT::MAP_OBJECT (OBJECT_HEAD Head) :
   m_pImpl (new Impl ()),
   Head (Head)
{
}

MAP_OBJECT::~MAP_OBJECT ()
{
   delete m_pImpl;
}

void MAP_OBJECT::Position (int64_t tmNow, double& dX, double& dY, double& dZ) const
{
   (void) tmNow;
   dX = Transform.d3Position[0];
   dY = Transform.d3Position[1];
   dZ = Transform.d3Position[2];
}

void MAP_OBJECT::Rotation (int64_t tmNow, double& dQx, double& dQy, double& dQz, double& dQw) const
{
   (void) tmNow;
   dQx = Transform.d4Rotation[0];
   dQy = Transform.d4Rotation[1];
   dQz = Transform.d4Rotation[2];
   dQw = Transform.d4Rotation[3];
}

void MAP_OBJECT::Scale (double& dX, double& dY, double& dZ) const
{
   dX = Transform.d3Scale[0];
   dY = Transform.d3Scale[1];
   dZ = Transform.d3Scale[2];
}

void MAP_OBJECT::Position (int64_t tmNow, VEC3& vPosition) const
{
   Position (tmNow, vPosition.dX, vPosition.dY, vPosition.dZ);
}

void MAP_OBJECT::Rotation (int64_t tmNow, QUAT& qRotation) const
{
   Rotation (tmNow, qRotation.dX, qRotation.dY, qRotation.dZ, qRotation.dW);
}

void MAP_OBJECT::Scale (VEC3& vScale) const
{
   Scale (vScale.dX, vScale.dY, vScale.dZ);
}

double MAP_OBJECT::Radius () const
{
   return Bound.d3Max[0];
}

uint32_t MAP_OBJECT::ColorToU32 () const
{
   uint32_t nColor;

   memcpy (&nColor, &Properties.Celestial.fColor, 4);

   return nColor & 0x00FFFFFF;
}

uint32_t MAP_OBJECT::ColorDimToU32 () const
{
   uint32_t nC = ColorToU32 ();
   int r = (nC >> 16) & 0xFF;
   int g = (nC >>  8) & 0xFF;
   int b =  nC        & 0xFF;
   return static_cast<uint32_t> (((r / 2) << 16) | ((g / 2) << 8) | (b / 2));
}

uint32_t MAP_OBJECT::ColorBrightToU32 () const
{
   uint32_t nC = ColorToU32 ();
   int r = (nC >> 16) & 0xFF;
   int g = (nC >>  8) & 0xFF;
   int b =  nC        & 0xFF;
   auto clamp = [] (int v) { return v > 255 ? 255 : v; };
   return static_cast<uint32_t> ((clamp (r + 64) << 16) | (clamp (g + 64) << 8) | clamp (b + 64));
}

bool MAP_OBJECT::GetTexture (const uint8_t* &pTex, int& nTexW, int& nTexH)
{
   return m_pImpl->GetTexture (pTex, nTexW, nTexH);
}

void MAP_OBJECT::SetTexture (const uint8_t* pTex, int nTexW, int nTexH)
{
   m_pImpl->SetTexture (pTex, nTexW, nTexH);
}

const GLTF_RENDER_MODEL* MAP_OBJECT::Gltf_Render_Model () const                    { return m_pImpl->Gltf_Render_Model (); }
void                     MAP_OBJECT::Gltf_Render_Model (GLTF_RENDER_MODEL* pModel) {        m_pImpl->Gltf_Render_Model (pModel); }

MAP_OBJECT::MAP_OBJECT_CLASS MAP_OBJECT::Class () const 
{ 
   return Head.Self.Class (); 
}

const char* MAP_OBJECT::ClassName (MAP_OBJECT_CLASS eType)
{
   const char* pcszResult;

   switch (eType)
   {
   case MAP_OBJECT::MAP_OBJECT_CLASS_ROOT:         pcszResult = "root";         break;
   case MAP_OBJECT::MAP_OBJECT_CLASS_CELESTIAL:    pcszResult = "celestial";    break;
   case MAP_OBJECT::MAP_OBJECT_CLASS_TERRESTRIAL:  pcszResult = "terrestrial";  break;
   case MAP_OBJECT::MAP_OBJECT_CLASS_PHYSICAL:     pcszResult = "physical";     break;
   case MAP_OBJECT::MAP_OBJECT_CLASS_PANEL:        pcszResult = "panel";        break;
   case MAP_OBJECT::MAP_OBJECT_CLASS_LIGHT:        pcszResult = "light";        break;
   default:                                        pcszResult = "";             break;
   }

   return pcszResult;
}


// ---------------------------------------------------------------------------
// MAP_OBJECT_ROOT
// ---------------------------------------------------------------------------

MAP_OBJECT_ROOT::MAP_OBJECT_ROOT (OBJECT_HEAD Head) : MAP_OBJECT (Head)
{
}

// ---------------------------------------------------------------------------
// MAP_OBJECT_CELESTIAL
// ---------------------------------------------------------------------------

MAP_OBJECT_CELESTIAL::MAP_OBJECT_CELESTIAL (OBJECT_HEAD Head) : MAP_OBJECT (Head)
{
}

bool MAP_OBJECT_CELESTIAL::HasOrbit () const
{
   return Orbit.Celestial.dA != 0.0  &&  Orbit.Celestial.tmPeriod != 0  &&  Transform.d4Rotation[3] != 0.0;
}

void MAP_OBJECT_CELESTIAL::Position (int64_t tmNow, double& dX, double& dY, double& dZ) const
{
   ORBIT_POSITION pos;

   if (PositionAtTick (tmNow, pos))
   {
      dX = pos.dX;
      dY = pos.dY;
      dZ = pos.dZ;
   }
   else
   {
      dX = Transform.d3Position[0];
      dY = Transform.d3Position[1];
      dZ = Transform.d3Position[2];
   }
}

void MAP_OBJECT_CELESTIAL::Rotation (int64_t tmNow, double& dQx, double& dQy, double& dQz, double& dQw) const
{
   uint8_t bType = Type.bType;

   if (bType == MAP_OBJECT_TYPE_TYPE_CELESTIAL_STAR
   ||  bType == MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANET
   ||  bType == MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOON
   ||  bType == MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRIS)
   {
      double eX = Transform.d4Rotation[0];
      double eY = Transform.d4Rotation[1];
      double eZ = Transform.d4Rotation[2];
      double eW = Transform.d4Rotation[3];

      if (eW == 0.0  &&  eX == 0.0  &&  eY == 0.0  &&  eZ == 0.0)
      {
         dQx = 0.0;  dQy = 0.0;  dQz = 0.0;  dQw = 1.0;
      }
      else
      {
         double dPrecX = Transform.d3Position[0];
         double dPrecY = Transform.d3Position[1];
         double dPrecZ = Transform.d3Position[2];
         double dRate  = std::sqrt (dPrecX * dPrecX + dPrecY * dPrecY + dPrecZ * dPrecZ);

         if (dRate > 1e-30  &&  tmNow != 0)
         {
            double dAngle   = dRate * static_cast<double> (tmNow);
            double dHalf    = dAngle * 0.5;
            double dSinHalf = std::sin (dHalf) / dRate;

            QUAT qPrec     = { dPrecX * dSinHalf, dPrecY * dSinHalf, dPrecZ * dSinHalf, std::cos (dHalf) };
            QUAT qComposed = QuatMultiply (qPrec, { eX, eY, eZ, eW });

            dQx = qComposed.dX;
            dQy = qComposed.dY;
            dQz = qComposed.dZ;
            dQw = qComposed.dW;
         }
         else
         {
            dQx = eX;  dQy = eY;  dQz = eZ;  dQw = eW;
         }
      }
   }
   else if (bType == MAP_OBJECT_TYPE_TYPE_CELESTIAL_SURFACE)
   {
      int64_t tmSpinPeriod = Orbit.Celestial.tmPeriod;

      if (tmSpinPeriod != 0)
      {
         double dW0Rad = Orbit.Celestial.dA;
         double dAngle = dW0Rad + (static_cast<double> (tmNow) / static_cast<double> (tmSpinPeriod)) * TWO_PI;
         double dHalf  = dAngle * 0.5;

         // Spin about the local polar axis = +Z (Z-up world).
         dQx = 0.0;
         dQy = 0.0;
         dQz = std::sin (dHalf);
         dQw = std::cos (dHalf);
      }
      else
      {
         dQx = 0.0;  dQy = 0.0;  dQz = 0.0;  dQw = 1.0;
      }
   }
   else
   {
      dQx = 0.0;  dQy = 0.0;  dQz = 0.0;  dQw = 1.0;
   }
}

bool MAP_OBJECT_CELESTIAL::PositionAtTick (int64_t tmNow, ORBIT_POSITION& out) const
{
   bool bResult = false;

   if (Orbit.Celestial.dA != 0.0  &&  Orbit.Celestial.tmPeriod != 0  &&  Transform.d4Rotation[3] != 0.0)
   {
      double dA   = Orbit.Celestial.dA;
      double dB   = Orbit.Celestial.dB;
      double dEcc = std::sqrt (1.0 - (dB * dB) / (dA * dA));

      int64_t tmInOrbit = ((Orbit.Celestial.tmOrigin + tmNow) % Orbit.Celestial.tmPeriod + Orbit.Celestial.tmPeriod) % Orbit.Celestial.tmPeriod;
      double  dM        = (static_cast<double> (tmInOrbit) / static_cast<double> (Orbit.Celestial.tmPeriod)) * TWO_PI;
      double  dE        = SolveKepler (dM, dEcc);

      double dRx = Transform.d4Rotation[0];
      double dRy = Transform.d4Rotation[1];
      double dRz = Transform.d4Rotation[2];
      double dRw = Transform.d4Rotation[3];

      if (tmNow != 0)
      {
         double dPrecX = Transform.d3Position[0];
         double dPrecY = Transform.d3Position[1];
         double dPrecZ = Transform.d3Position[2];
         double dRate = std::sqrt (dPrecX * dPrecX + dPrecY * dPrecY + dPrecZ * dPrecZ);

         if (dRate > 1e-30)
         {
            double dAngle   = dRate * static_cast<double> (tmNow);
            double dHalf    = dAngle * 0.5;
            double dSinHalf = std::sin (dHalf) / dRate;

            QUAT pPrec = { dPrecX * dSinHalf, dPrecY * dSinHalf, dPrecZ * dSinHalf, std::cos (dHalf) };
            QUAT pComposed = QuatMultiply (pPrec, { dRx, dRy, dRz, dRw });
            dRx = pComposed.dX;
            dRy = pComposed.dY;
            dRz = pComposed.dZ;
            dRw = pComposed.dW;
         }
      }

      double dLX = dA * (std::cos (dE) - dEcc);
      double dLY = dB * std::sin (dE);

      // Orbit lies in the local XY plane (Z-up world): perihelion on +X, sweeping
      // toward +Y as E grows (prograde / counter-clockwise seen from +Z). The
      // orientation quaternion tilts this plane into the reference frame.
      VEC3 pPos = RotateByQuat (dRx, dRy, dRz, dRw, dLX, dLY, 0.0);

      out.dX = pPos.dX;
      out.dY = pPos.dY;
      out.dZ = pPos.dZ;
      out.dE = dE;

      bResult = true;
   }

   return bResult;
}

VEC3 MAP_OBJECT_CELESTIAL::OrbitTrailPoint (double dE, int64_t tmElapsed) const
{
   double dRx = Transform.d4Rotation[0];
   double dRy = Transform.d4Rotation[1];
   double dRz = Transform.d4Rotation[2];
   double dRw = Transform.d4Rotation[3];

   if (tmElapsed != 0)
   {
      double dPrecX = Transform.d3Position[0];
      double dPrecY = Transform.d3Position[1];
      double dPrecZ = Transform.d3Position[2];
      double dRate = std::sqrt (dPrecX * dPrecX + dPrecY * dPrecY + dPrecZ * dPrecZ);

      if (dRate > 1e-30)
      {
         double dAngle   = dRate * static_cast<double> (tmElapsed);
         double dHalf    = dAngle * 0.5;
         double dSinHalf = std::sin (dHalf) / dRate;

         QUAT pPrec = { dPrecX * dSinHalf, dPrecY * dSinHalf, dPrecZ * dSinHalf, std::cos (dHalf) };
         QUAT pComposed = QuatMultiply (pPrec, { dRx, dRy, dRz, dRw });
         dRx = pComposed.dX;
         dRy = pComposed.dY;
         dRz = pComposed.dZ;
         dRw = pComposed.dW;
      }
   }

   double dA   = Orbit.Celestial.dA;
   double dB   = Orbit.Celestial.dB;
   double dEcc = std::sqrt (1.0 - (dB * dB) / (dA * dA));
   double dLX  = dA * (std::cos (dE) - dEcc);
   double dLY  = dB * std::sin (dE);

   return RotateByQuat (dRx, dRy, dRz, dRw, dLX, dLY, 0.0);
}

const char* MAP_OBJECT_CELESTIAL::GetTypeName (MAP_OBJECT_TYPE_TYPE_CELESTIAL eType)
{
   const char* pcszResult;

   switch (eType)
   {
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_NONE:          pcszResult = "none";              break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_UNIVERSE:      pcszResult = "universe";          break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_SUPERCLUSTER:  pcszResult = "supercluster";      break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_GALAXYCLUSTER: pcszResult = "galaxycluster";     break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_GALAXY:        pcszResult = "galaxy";            break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_SECTOR:        pcszResult = "sector";            break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_NEBULA:        pcszResult = "nebula";            break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARCLUSTER:   pcszResult = "starcluster";       break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_BLACKHOLE:     pcszResult = "blackhole";         break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARSYSTEM:    pcszResult = "starsystem";        break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_STAR:          pcszResult = "star";              break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANETSYSTEM:  pcszResult = "planetsystem";      break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANET:        pcszResult = "planet";            break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOONSYSTEM:    pcszResult = "moonsystem";        break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOON:          pcszResult = "moon";              break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRISSYSTEM:  pcszResult = "debrissystem";      break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRIS:        pcszResult = "debris";            break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_SATELLITE:     pcszResult = "satellite";         break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_TRANSPORT:     pcszResult = "transport";         break;
   case MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_SURFACE:       pcszResult = "surface";           break;

   default:
      pcszResult = "";
   }

   return pcszResult;
}

// ---------------------------------------------------------------------------
// MAP_OBJECT_TERRESTRIAL
// ---------------------------------------------------------------------------

MAP_OBJECT_TERRESTRIAL::MAP_OBJECT_TERRESTRIAL (OBJECT_HEAD Head) : MAP_OBJECT (Head)
{
}

// ---------------------------------------------------------------------------
// MAP_OBJECT_PHYSICAL
// ---------------------------------------------------------------------------

MAP_OBJECT_PHYSICAL::MAP_OBJECT_PHYSICAL (OBJECT_HEAD Head) : MAP_OBJECT (Head)
{
}

// ---------------------------------------------------------------------------
// MAP_OBJECT_LIGHT
// ---------------------------------------------------------------------------

MAP_OBJECT_LIGHT::MAP_OBJECT_LIGHT (OBJECT_HEAD Head) : MAP_OBJECT (Head)
{
}

// ---------------------------------------------------------------------------
// MAP_OBJECT_PANEL
// ---------------------------------------------------------------------------

MAP_OBJECT_PANEL::MAP_OBJECT_PANEL (OBJECT_HEAD Head) :
   MAP_OBJECT (Head),
   m_pPanel (new DEP::UI_PANEL ())
{
}

MAP_OBJECT_PANEL::~MAP_OBJECT_PANEL ()
{
   delete m_pPanel;
}

void MAP_OBJECT_PANEL::Source (const std::string& sSource)
{
   m_pPanel->Source (sSource);
}

bool MAP_OBJECT_PANEL::Render (ENGINE* pEngine, int nWidth, int nHeight)
{
   return m_pPanel->Render (pEngine, nWidth, nHeight);
}

const uint8_t* MAP_OBJECT_PANEL::Pixels () const
{
   return m_pPanel->Pixels ();
}

int MAP_OBJECT_PANEL::Width () const
{
   return m_pPanel->Width ();
}

int MAP_OBJECT_PANEL::Height () const
{
   return m_pPanel->Height ();
}
