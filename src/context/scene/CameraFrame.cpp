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

#include "CameraFrame.h"
#include "UrlResolve.h"
#include "Viewport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace SNEEZE;

namespace
{

   std::string Trim (const std::string& sIn)
   {
      size_t nBegin = 0;
      size_t nEnd   = sIn.length ();

      while (nBegin < nEnd  &&  (sIn[nBegin] == ' '  ||  sIn[nBegin] == '\t'))
         nBegin++;

      while (nEnd > nBegin  &&  (sIn[nEnd - 1] == ' '  ||  sIn[nEnd - 1] == '\t'))
         nEnd--;

      return sIn.substr (nBegin, nEnd - nBegin);
   }

   std::string Lower (const std::string& sIn)
   {
      std::string sOut = sIn;

      for (char& c : sOut)
      {
         if (c >= 'A'  &&  c <= 'Z')
            c = static_cast<char> (c - 'A' + 'a');
      }

      return sOut;
   }

   bool IsFalseWord (const std::string& sLower)
   {
      return sLower == "false"  ||  sLower == "off"  ||  sLower == "no";
   }

   bool IsTrueWord (const std::string& sLower)
   {
      return sLower == "true"  ||  sLower == "on"  ||  sLower == "yes"  ||  sLower == "auto";
   }

   // nCount numbers separated by commas. The last number consumes the rest of
   // the string, so a trailing comma or an extra field fails.
   bool ParseDoubles (const std::string& sIn, double* aOut, int nCount)
   {
      bool   bOk    = (nCount > 0  &&  aOut != nullptr);
      size_t nStart = 0;

      for (int i = 0; bOk  &&  i < nCount; i++)
      {
         size_t nComma = sIn.find (',', nStart);
         bool   bLast  = (i == nCount - 1);
         size_t nEnd   = (!bLast  &&  nComma != std::string::npos) ? nComma : sIn.length ();

         if (!bLast  &&  nComma == std::string::npos)
            bOk = false;
         else if (bLast  &&  nComma != std::string::npos)
            bOk = false;
         else
         {
            std::string sPart = Trim (sIn.substr (nStart, nEnd - nStart));
            char*       pEnd  = nullptr;
            double      dValue = std::strtod (sPart.c_str (), &pEnd);

            if (sPart.empty ()  ||  pEnd == sPart.c_str ()  ||  *pEnd != '\0')
               bOk = false;
            else
               aOut[i] = dValue;

            nStart = nEnd + 1;
         }
      }

      return bOk;
   }

   void ApplyBounds (CAMERA_FRAME& Frame, const nlohmann::json& jValue)
   {
      if (jValue.is_boolean ())
         Frame.bBounds = jValue.get<bool> ();
      else if (jValue.is_number ())
         Frame.bBounds = (jValue.get<double> () != 0.0);
      else if (jValue.is_string ())
      {
         std::string sLower = Lower (Trim (jValue.get<std::string> ()));

         if (IsFalseWord (sLower)  ||  sLower == "0")
            Frame.bBounds = false;
         else if (IsTrueWord (sLower)  ||  sLower == "1")
            Frame.bBounds = true;
      }
   }

   void ApplyScale (CAMERA_FRAME& Frame, const nlohmann::json& jValue)
   {
      if (jValue.is_boolean ())
      {
         Frame.bScale    = jValue.get<bool> ();
         Frame.bScaleSet = false;

         if (!Frame.bScale)
            Frame.dScale = 1.0;
      }
      else if (jValue.is_number ())
      {
         double dValue = jValue.get<double> ();

         if (dValue > 0.0)
         {
            Frame.bScale    = false;
            Frame.bScaleSet = true;
            Frame.dScale    = dValue;
         }
         else
         {
            Frame.bScale    = false;
            Frame.bScaleSet = false;
            Frame.dScale    = 1.0;
         }
      }
      else if (jValue.is_string ())
      {
         std::string sLower = Lower (Trim (jValue.get<std::string> ()));
         double      aValue[1];

         if (IsFalseWord (sLower)  ||  sLower == "0")
         {
            Frame.bScale    = false;
            Frame.bScaleSet = false;
            Frame.dScale    = 1.0;
         }
         else if (IsTrueWord (sLower))
         {
            Frame.bScale    = true;
            Frame.bScaleSet = false;
         }
         else if (ParseDoubles (sLower, aValue, 1)  &&  aValue[0] > 0.0)
         {
            Frame.bScale    = false;
            Frame.bScaleSet = true;
            Frame.dScale    = aValue[0];
         }
      }
   }

   void ApplyVector (CAMERA_FRAME& Frame, const nlohmann::json& jValue, int nCount, bool bPosition)
   {
      double aValue[4] = { 0.0, 0.0, 0.0, 1.0 };
      bool   bOk       = false;

      if (jValue.is_array ()  &&  static_cast<int> (jValue.size ()) == nCount)
      {
         bOk = true;

         for (int i = 0; bOk  &&  i < nCount; i++)
         {
            if (!jValue[i].is_number ())
               bOk = false;
            else
               aValue[i] = jValue[i].get<double> ();
         }
      }
      else if (jValue.is_string ())
         bOk = ParseDoubles (jValue.get<std::string> (), aValue, nCount);

      if (bOk  &&  bPosition)
      {
         Frame.bPosition    = true;
         Frame.aPosition[0] = aValue[0];
         Frame.aPosition[1] = aValue[1];
         Frame.aPosition[2] = aValue[2];
      }
      else if (bOk)
      {
         Frame.bRotation    = true;
         Frame.aRotation[0] = aValue[0];
         Frame.aRotation[1] = aValue[1];
         Frame.aRotation[2] = aValue[2];
         Frame.aRotation[3] = aValue[3];
      }
   }

   void ApplyKey (CAMERA_FRAME& Frame, const std::string& sNameIn, const nlohmann::json& jValue)
   {
      std::string sName = Lower (Trim (sNameIn));

      if (sName == "bounds"  ||  sName == "bbounds")
         ApplyBounds (Frame, jValue);
      else if (sName == "scale"  ||  sName == "dscale")
         ApplyScale (Frame, jValue);
      else if (sName == "position")
         ApplyVector (Frame, jValue, 3, true);
      else if (sName == "rotation")
         ApplyVector (Frame, jValue, 4, false);
   }

   void ApplyObject (CAMERA_FRAME& Frame, const nlohmann::json& jObject)
   {
      if (jObject.is_object ())
      {
         for (auto it = jObject.begin (); it != jObject.end (); ++it)
            ApplyKey (Frame, it.key (), it.value ());
      }
   }

   void ApplyQuery (CAMERA_FRAME& Frame, const std::string& sQuery)
   {
      size_t nStart = 0;

      while (nStart <= sQuery.length ())
      {
         size_t      nAmp   = sQuery.find ('&', nStart);
         size_t      nEnd   = (nAmp == std::string::npos) ? sQuery.length () : nAmp;
         std::string sToken = sQuery.substr (nStart, nEnd - nStart);
         size_t      nEq    = sToken.find ('=');

         if (nEq != std::string::npos  &&  nEq > 0)
         {
            std::string sName  = sToken.substr (0, nEq);
            std::string sValue = sToken.substr (nEq + 1);

            if (!Trim (sName).empty ()  &&  !Trim (sValue).empty ())
               ApplyKey (Frame, sName, nlohmann::json (sValue));
         }

         if (nAmp == std::string::npos)
            nStart = sQuery.length () + 1;
         else
            nStart = nEnd + 1;
      }
   }

   const nlohmann::json* FindObject (const nlohmann::json& jObject, const char* sName)
   {
      const nlohmann::json* pFound = nullptr;

      if (jObject.is_object ())
      {
         for (auto it = jObject.begin (); pFound == nullptr  &&  it != jObject.end (); ++it)
         {
            if (Lower (it.key ()) == sName  &&  it.value ().is_object ())
               pFound = &it.value ();
         }
      }

      return pFound;
   }

   // Identity-forward is +X (Z-up). The orbit stores the eye as a distance and
   // two angles around a target, so the absolute pose is converted back into
   // that form. The direction inversion (look = -orbit offset) matches the
   // orbit used by the compositor and is left as it was.
   void Seed (VIEWPORT* pViewport, const VIEWPORT::CAMERA& CameraPose, double dRenderScale)
   {
      VIEWPORT::VIEW& View = pViewport->View ();

      RMAP::MAP::MAP_OBJECT::VEC3 vEye = { CameraPose.aPosition[0] * dRenderScale, CameraPose.aPosition[1] * dRenderScale, CameraPose.aPosition[2] * dRenderScale };
      RMAP::MAP::MAP_OBJECT::QUAT qPose = { CameraPose.aRotation[0], CameraPose.aRotation[1], CameraPose.aRotation[2], CameraPose.aRotation[3] };
      RMAP::MAP::MAP_OBJECT::VEC3 vT;

      vT.dX = 2.0 * (qPose.dY * 0.0 - qPose.dZ * 0.0);
      vT.dY = 2.0 * (qPose.dZ * 1.0 - qPose.dX * 0.0);
      vT.dZ = 2.0 * (qPose.dX * 0.0 - qPose.dY * 1.0);

      RMAP::MAP::MAP_OBJECT::VEC3 vDir;

      vDir.dX = 1.0 + qPose.dW * vT.dX + (qPose.dY * vT.dZ - qPose.dZ * vT.dY);
      vDir.dY = 0.0 + qPose.dW * vT.dY + (qPose.dZ * vT.dX - qPose.dX * vT.dZ);
      vDir.dZ = 0.0 + qPose.dW * vT.dZ + (qPose.dX * vT.dY - qPose.dY * vT.dX);

      double dLen = vDir.Length ();

      if (dLen > 1e-9)
      {
         vDir.dX /= dLen;
         vDir.dY /= dLen;
         vDir.dZ /= dLen;
      }

      double dDistance = vEye.Length ();

      if (dDistance < 1e-6)
         dDistance = 10.0;

      View.m_vTarget   = vEye + vDir * dDistance;
      View.m_dDistance = static_cast<float> (dDistance);
      View.m_dPhi      = static_cast<float> (std::asin (std::max (-1.0, std::min (1.0, -vDir.dZ))));
      View.m_dTheta    = static_cast<float> (std::atan2 (-vDir.dY, -vDir.dX));
   }

}

void SNEEZE::CameraFrame_Read (CAMERA_FRAME& Frame, const std::string& sPayload, const std::string& sUrl)
{
   nlohmann::json jPayload = nlohmann::json::object ();
   bool           bParsed  = false;

   if (!sPayload.empty ())
   {
      try
      {
         jPayload = nlohmann::json::parse (sPayload);
         bParsed  = true;
      }
      catch (const nlohmann::json::exception&)
      {
      }
   }

   if (bParsed)
   {
      const nlohmann::json* pData   = FindObject (jPayload, "data");
      const nlohmann::json* pCamera = pData ? FindObject (*pData, "camera") : nullptr;

      if (pCamera)
         ApplyObject (Frame, *pCamera);
   }

   UrlResolve::PARTS Parts = UrlResolve::Parse (sUrl);

   if (Parts.bQuery)
      ApplyQuery (Frame, Parts.sQuery);
}

void SNEEZE::CameraFrame_Bind (const CAMERA_FRAME& Frame, VIEWPORT* pViewport)
{
   if (pViewport  &&  (Frame.bPosition  ||  Frame.bRotation))
   {
      VIEWPORT::CAMERA Camera = pViewport->Camera ();

      if (Frame.bPosition)
      {
         Camera.aPosition[0] = Frame.aPosition[0];
         Camera.aPosition[1] = Frame.aPosition[1];
         Camera.aPosition[2] = Frame.aPosition[2];
      }

      if (Frame.bRotation)
      {
         Camera.aRotation[0] = Frame.aRotation[0];
         Camera.aRotation[1] = Frame.aRotation[1];
         Camera.aRotation[2] = Frame.aRotation[2];
         Camera.aRotation[3] = Frame.aRotation[3];
      }

      pViewport->Camera (Camera);
   }
}

double SNEEZE::CameraFrame_Scale (const CAMERA_FRAME& Frame, double dMaxReach, double dTargetExtent, double dMinReach)
{
   double dScale = 1.0;

   if (Frame.bScaleSet)
      dScale = Frame.dScale;
   else if (Frame.bScale  &&  dMaxReach > dMinReach)
      dScale = dTargetExtent / dMaxReach;

   return dScale;
}

void SNEEZE::CameraFrame_Pose (const CAMERA_FRAME& Frame, VIEWPORT* pViewport, double dRenderScale, double dMaxReach, double dMinReach, bool& bHeld)
{
   if (pViewport)
   {
      bool bReady = Frame.bScaleSet  ||  !Frame.bScale  ||  dMaxReach > dMinReach;

      if (Frame.bBounds)
      {
         VIEWPORT::CAMERA CameraPose;

         bHeld = false;

         if (bReady  &&  pViewport->Camera_Active (CameraPose))
            Seed (pViewport, CameraPose, dRenderScale);
      }
      else if (!bHeld)
      {
         VIEWPORT::CAMERA CameraPose;

         if (bReady  &&  pViewport->Camera_Active (CameraPose))
         {
            Seed (pViewport, CameraPose, dRenderScale);
            bHeld = true;
         }
      }
   }
}
