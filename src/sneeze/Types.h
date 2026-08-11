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

#ifndef SNEEZE_CORE_TYPES_H
#define SNEEZE_CORE_TYPES_H

// Layout-compatible with float[3] / ANARI_FLOAT32_VEC3, so it passes straight
// to anariSetParameter as &rgb.
struct RGB
{
   float fR;
   float fG;
   float fB;

   RGB operator* (float fScale) const;
};

// RGB plus alpha; layout-compatible with float[4] / ANARI_FLOAT32_VEC4.
struct RGBA
{
   float fR;
   float fG;
   float fB;
   float fA;
};

// Column-major 4x4 (translation in d[12..14]), matching ANARI_FLOAT32_MAT4 layout.
struct MAT4
{
   double d[16];
};

// Float sibling of MAT4 (render space); layout-compatible with ANARI_FLOAT32_MAT4.
struct MAT4F
{
   float f[16];
};

// A width/height pair (unsuffixed = int; F = float; D = double).
struct DIM2
{
   int nW;
   int nH;
};

struct DIM2F
{
   float fW;
   float fH;
};

struct DIM2D
{
   double dW;
   double dH;
};

constexpr double PI         = 3.14159265358979323846;
constexpr double TWO_PI     = 2.0 * PI;
constexpr double DEG_TO_RAD = PI / 180.0;

constexpr double AU_M         = 149597870700.0;
constexpr double GM_SUN_M3S2  = 1.32712440041279419e20;
constexpr double JULIAN_YEAR  = 365.25 * 86400.0;
constexpr double G_M3_KG_S2   = 6.67430e-11;

constexpr int64_t TICKS_PER_S  = 64;
constexpr int64_t TICKS_PER_CY = 36525LL * 86400LL * TICKS_PER_S;

constexpr double OBLIQUITY_J2000 = 23.4392911;

#endif // SNEEZE_CORE_TYPES_H
