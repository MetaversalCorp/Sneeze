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

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
// Suite entry points (defined in their respective translation units)
// ---------------------------------------------------------------------------

#ifdef SNEEZE_HAS_WASM
extern int RunWasmTests    (int nArgc, char** aArgv);
extern int RunChronoTests  (int nArgc, char** aArgv);
#endif
extern int RunSpvTests     (int nArgc, char** aArgv);
#ifdef SNEEZE_ENABLE_XR
extern int RunXrTests      (int nArgc, char** aArgv);
#endif
extern int RunNetTests     (int nArgc, char** aArgv);
extern int RunUiTests      (int nArgc, char** aArgv);
extern int RunComputeTests (int nArgc, char** aArgv);
extern int RunVoxTests     (int nArgc, char** aArgv);
extern int RunJwsTests     (int nArgc, char** aArgv);
extern int RunNetworkTests (int nArgc, char** aArgv);
extern int RunStorageTests (int nArgc, char** aArgv);
extern int RunConsoleTests (int nArgc, char** aArgv);
extern int RunGltfTests    (int nArgc, char** aArgv);

// ---------------------------------------------------------------------------
// Suite table
// ---------------------------------------------------------------------------

struct SUITE
{
   const char*                           szFlag;
   const char*                           szName;
   std::function<int (int, char**)>      pfnRun;
};

static const SUITE g_aSuites[] =
{
#ifdef SNEEZE_HAS_WASM
   { "--wasm",    "Wasm",    RunWasmTests    },
   { "--chrono",  "Chrono",  RunChronoTests  },
#endif
   { "--spv",     "Spv",     RunSpvTests     },
#ifdef SNEEZE_ENABLE_XR
   { "--xr",      "Xr",      RunXrTests      },
#endif
   { "--net",     "Net",     RunNetTests     },
   { "--ui",      "Ui",      RunUiTests      },
   { "--compute", "Compute", RunComputeTests },
   { "--vox",     "Vox",     RunVoxTests     },
   { "--jws",     "Jws",     RunJwsTests     },
   { "--network", "Network", RunNetworkTests },
   { "--storage", "Storage", RunStorageTests },
   { "--console", "Console", RunConsoleTests },
   { "--gltf",    "Gltf",    RunGltfTests    },
};

static const int g_nSuiteCount = sizeof (g_aSuites) / sizeof (g_aSuites[0]);

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main (int nArgc, char** aArgv)
{
   std::vector<const SUITE*> aSelected;

   // Build a filtered argv that strips suite-selection flags so suite
   // functions that inspect argv (e.g. JwsTest reading a certs dir)
   // don't see them.
   std::vector<char*> aPassthrough;
   aPassthrough.push_back (aArgv[0]);

   for (int nArg = 1; nArg < nArgc; ++nArg)
   {
      if (std::strcmp (aArgv[nArg], "--help") == 0  ||  std::strcmp (aArgv[nArg], "-h") == 0)
      {
         std::printf ("Usage: SneezeTest [--suite ...]\n\n");
         std::printf ("Suites:\n");
         for (int nSuite = 0; nSuite < g_nSuiteCount; ++nSuite)
            std::printf ("  %-10s  %s\n", g_aSuites[nSuite].szFlag, g_aSuites[nSuite].szName);
         std::printf ("\nWith no flags, all suites run.\n");
         return 0;
      }

      bool bIsSuite = false;
      for (int nSuite = 0; nSuite < g_nSuiteCount; ++nSuite)
      {
         if (std::strcmp (aArgv[nArg], g_aSuites[nSuite].szFlag) == 0)
         {
            aSelected.push_back (&g_aSuites[nSuite]);
            bIsSuite = true;
         }
      }

      if (!bIsSuite)
         aPassthrough.push_back (aArgv[nArg]);
   }

   bool bRunAll = aSelected.empty ();
   if (bRunAll)
   {
      for (int nSuite = 0; nSuite < g_nSuiteCount; ++nSuite)
         aSelected.push_back (&g_aSuites[nSuite]);
   }

   int nPassArgc = static_cast<int> (aPassthrough.size ());
   char** aPassArgv = aPassthrough.data ();

   std::printf ("SneezeTest - running %d suite(s)\n", static_cast<int> (aSelected.size ()));
   std::printf ("=========================================================\n\n");

   int nFailed = 0;

   for (size_t nSuite = 0; nSuite < aSelected.size (); ++nSuite)
   {
      int nResult = aSelected[nSuite]->pfnRun (nPassArgc, aPassArgv);
      if (nResult != 0)
         nFailed++;
      std::printf ("\n");
   }

   std::printf ("=========================================================\n");
   std::printf ("SneezeTest - %d/%d suites passed\n",
      static_cast<int> (aSelected.size ()) - nFailed,
      static_cast<int> (aSelected.size ()));
   std::printf ("=========================================================\n");

   return (nFailed > 0) ? 1 : 0;
}
   