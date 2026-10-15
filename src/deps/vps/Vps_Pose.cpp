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

#include "vps/Vps.h"

#include <cmath>
#include <string>
#include <nlohmann/json.hpp>

namespace
{
   void Mat_From_Quat (double dX, double dY, double dZ, double dW, double aR[3][3])
   {
      double dXX = dX * dX;
      double dYY = dY * dY;
      double dZZ = dZ * dZ;
      double dXY = dX * dY;
      double dXZ = dX * dZ;
      double dYZ = dY * dZ;
      double dWX = dW * dX;
      double dWY = dW * dY;
      double dWZ = dW * dZ;

      aR[0][0] = 1.0 - 2.0 * (dYY + dZZ);
      aR[0][1] = 2.0 * (dXY - dWZ);
      aR[0][2] = 2.0 * (dXZ + dWY);
      aR[1][0] = 2.0 * (dXY + dWZ);
      aR[1][1] = 1.0 - 2.0 * (dXX + dZZ);
      aR[1][2] = 2.0 * (dYZ - dWX);
      aR[2][0] = 2.0 * (dXZ - dWY);
      aR[2][1] = 2.0 * (dYZ + dWX);
      aR[2][2] = 1.0 - 2.0 * (dXX + dYY);
   }

   void Mat_Mul (const double aA[3][3], const double aB[3][3], double aC[3][3])
   {
      for (int nRow = 0; nRow < 3; ++nRow)
      {
         for (int nCol = 0; nCol < 3; ++nCol)
         {
            aC[nRow][nCol] = aA[nRow][0] * aB[0][nCol] + aA[nRow][1] * aB[1][nCol] + aA[nRow][2] * aB[2][nCol];
         }
      }
   }

   void Quat_From_Mat (const double aR[3][3], double& dX, double& dY, double& dZ, double& dW)
   {
      double dTrace = aR[0][0] + aR[1][1] + aR[2][2];
      double dS     = 0.0;

      if (dTrace > 0.0)
      {
         dS = std::sqrt (dTrace + 1.0) * 2.0;
         dW = 0.25 * dS;
         dX = (aR[2][1] - aR[1][2]) / dS;
         dY = (aR[0][2] - aR[2][0]) / dS;
         dZ = (aR[1][0] - aR[0][1]) / dS;
      }
      else if (aR[0][0] > aR[1][1]  &&  aR[0][0] > aR[2][2])
      {
         dS = std::sqrt (1.0 + aR[0][0] - aR[1][1] - aR[2][2]) * 2.0;
         dW = (aR[2][1] - aR[1][2]) / dS;
         dX = 0.25 * dS;
         dY = (aR[0][1] + aR[1][0]) / dS;
         dZ = (aR[0][2] + aR[2][0]) / dS;
      }
      else if (aR[1][1] > aR[2][2])
      {
         dS = std::sqrt (1.0 + aR[1][1] - aR[0][0] - aR[2][2]) * 2.0;
         dW = (aR[0][2] - aR[2][0]) / dS;
         dX = (aR[0][1] + aR[1][0]) / dS;
         dY = 0.25 * dS;
         dZ = (aR[1][2] + aR[2][1]) / dS;
      }
      else
      {
         dS = std::sqrt (1.0 + aR[2][2] - aR[0][0] - aR[1][1]) * 2.0;
         dW = (aR[1][0] - aR[0][1]) / dS;
         dX = (aR[0][2] + aR[2][0]) / dS;
         dY = (aR[1][2] + aR[2][1]) / dS;
         dZ = 0.25 * dS;
      }

      double dLen = std::sqrt (dX * dX + dY * dY + dZ * dZ + dW * dW);
      if (dLen > 1e-12)
      {
         dX /= dLen;
         dY /= dLen;
         dZ /= dLen;
         dW /= dLen;
      }
      else
      {
         dX = 0.0;
         dY = 0.0;
         dZ = 0.0;
         dW = 1.0;
      }
   }

   bool Json_Double (const nlohmann::json& jValue, double& dOut)
   {
      bool bOk = false;

      if (jValue.is_number ())
      {
         dOut = jValue.get<double> ();
         bOk  = true;
      }
      else if (jValue.is_string ())
      {
         try
         {
            dOut = std::stod (jValue.get<std::string> ());
            bOk  = true;
         }
         catch (...)
         {
            bOk = false;
         }
      }

      return bOk;
   }

   const nlohmann::json* Fix_Find (const nlohmann::json& jBody)
   {
      const nlohmann::json* pFix = nullptr;

      if (jBody.is_array ()  &&  !jBody.empty ()  &&  jBody[0].is_object ())
         pFix = &jBody[0];
      else if (jBody.is_object ())
      {
         if (jBody.contains ("camera_from_map_transform"))
            pFix = &jBody;
         else if (jBody.contains ("localizations")  &&  jBody["localizations"].is_array ()  &&  !jBody["localizations"].empty ()  &&  jBody["localizations"][0].is_object ())
            pFix = &jBody["localizations"][0];
      }

      if (pFix  &&  !pFix->contains ("camera_from_map_transform"))
         pFix = nullptr;

      return pFix;
   }

   double Confidence_Read (const nlohmann::json& jFix)
   {
      double dConfidence = 0.0;

      if (jFix.contains ("metrics")  &&  jFix["metrics"].is_object ())
      {
         const nlohmann::json& jMetrics = jFix["metrics"];
         if (!jMetrics.contains ("confidence_tight")  ||  !Json_Double (jMetrics["confidence_tight"], dConfidence))
         {
            if (jMetrics.contains ("confidence"))
               Json_Double (jMetrics["confidence"], dConfidence);
         }
      }

      return dConfidence;
   }
}

bool SNEEZE::DEP::Pose_From_Opencv (double dX, double dY, double dZ, double dQx, double dQy, double dQz, double dQw, VPS::POSE& Pose)
{
   // OpenCV world (X right, Y down, Z forward) to Sneeze (X east, Y north, Z up),
   // and OpenCV camera axes (X right, Y down, Z forward) to the Sneeze camera
   // (X forward, Y right, Z up). Identity OpenCV looking +Z becomes identity
   // Sneeze looking +X.
   const double aB[3][3] = {
      { 0.0,  0.0, 1.0 },
      { 1.0,  0.0, 0.0 },
      { 0.0, -1.0, 0.0 },
   };
   const double aC[3][3] = {
      { 0.0, 1.0,  0.0 },
      { 0.0, 0.0, -1.0 },
      { 1.0, 0.0,  0.0 },
   };

   double aRo[3][3];
   double aTmp[3][3];
   double aRs[3][3];
   bool   bOk = true;

   Mat_From_Quat (dQx, dQy, dQz, dQw, aRo);
   Mat_Mul (aRo, aC, aTmp);
   Mat_Mul (aB, aTmp, aRs);

   Pose.aPosition[0] = aB[0][0] * dX + aB[0][1] * dY + aB[0][2] * dZ;
   Pose.aPosition[1] = aB[1][0] * dX + aB[1][1] * dY + aB[1][2] * dZ;
   Pose.aPosition[2] = aB[2][0] * dX + aB[2][1] * dY + aB[2][2] * dZ;
   Quat_From_Mat (aRs, Pose.aRotation[0], Pose.aRotation[1], Pose.aRotation[2], Pose.aRotation[3]);
   Pose.bValid = bOk;

   return bOk;
}

bool SNEEZE::DEP::Pose_From_Placeframe (const std::string& sJson, VPS::POSE& Pose)
{
   bool bOk = false;

   Pose.bValid      = false;
   Pose.dConfidence = 0.0;

   try
   {
      nlohmann::json jBody = nlohmann::json::parse (sJson);
      const nlohmann::json* pFix = Fix_Find (jBody);

      if (pFix)
      {
         const nlohmann::json& jPose = (*pFix)["camera_from_map_transform"];
         double dX = 0.0, dY = 0.0, dZ = 0.0, dQx = 0.0, dQy = 0.0, dQz = 0.0, dQw = 1.0;

         bOk = jPose.contains ("translation")  &&  jPose.contains ("rotation")
            &&  Json_Double (jPose["translation"].at ("x"), dX)
            &&  Json_Double (jPose["translation"].at ("y"), dY)
            &&  Json_Double (jPose["translation"].at ("z"), dZ)
            &&  Json_Double (jPose["rotation"].at ("x"), dQx)
            &&  Json_Double (jPose["rotation"].at ("y"), dQy)
            &&  Json_Double (jPose["rotation"].at ("z"), dQz)
            &&  Json_Double (jPose["rotation"].at ("w"), dQw);

         if (bOk)
         {
            bOk = Pose_From_Opencv (dX, dY, dZ, dQx, dQy, dQz, dQw, Pose);
            Pose.dConfidence = Confidence_Read (*pFix);
         }
      }
   }
   catch (...)
   {
      bOk         = false;
      Pose.bValid = false;
   }

   return bOk;
}
