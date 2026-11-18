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

#include "context/scene/UrlResolve.h"

#include <cstdio>
#include <string>

static int g_nTests  = 0;
static int g_nPassed = 0;

static void Check (const char* sBase, const char* sRef, const char* sExpect)
{
   std::string sGot = SNEEZE::UrlResolve::Resolve (sBase, sRef);

   g_nTests++;

   if (sGot == sExpect)
   {
      g_nPassed++;
      printf ("  PASS: [%s] + [%s] -> %s\n", sBase, sRef, sGot.c_str ());
   }
   else
   {
      printf ("  FAIL: [%s] + [%s] -> %s (expected %s)\n", sBase, sRef, sGot.c_str (), sExpect);
   }
}

int RunUrlTests (int nArgc, char** aArgv)
{
   (void) nArgc;
   (void) aArgv;

   const char* sBase = "https://h.com/a/b/c.msf?x=1&y=2#top";

   printf ("\n--- Relative paths ---\n");
   Check (sBase, "d.msf",          "https://h.com/a/b/d.msf");
   Check (sBase, "./d.msf",        "https://h.com/a/b/d.msf");
   Check (sBase, "../d.msf",       "https://h.com/a/d.msf");
   Check (sBase, "../../../../d",  "https://h.com/d");
   Check (sBase, "/d.msf",         "https://h.com/d.msf");
   Check ("https://h.com",   "d.msf", "https://h.com/d.msf");
   Check ("https://h.com?q", "d.msf", "https://h.com/d.msf");

   printf ("\n--- Slashes and dot segments ---\n");
   Check (sBase, "dir/",           "https://h.com/a/b/dir/");
   Check (sBase, "dir/..",         "https://h.com/a/b/");
   Check (sBase, "dir/.",          "https://h.com/a/b/dir/");
   Check (sBase, "a//b",           "https://h.com/a/b/a//b");
   Check (sBase, "%2e%2e/d",       "https://h.com/a/b/%2e%2e/d");
   Check (sBase, ".%2E/d",         "https://h.com/a/b/.%2E/d");
   Check (sBase, "%2e./d",         "https://h.com/a/b/%2e./d");
   Check (sBase, "%2E/d",          "https://h.com/a/b/%2E/d");
   Check (sBase, "%2e/../d",       "https://h.com/a/b/d");

   printf ("\n--- Empty path stays on the base document ---\n");
   Check (sBase, "",               "https://h.com/a/b/c.msf?x=1&y=2");
   Check (sBase, "#frag",          "https://h.com/a/b/c.msf?x=1&y=2#frag");
   Check (sBase, "?z=3",           "https://h.com/a/b/c.msf?z=3");
   Check (sBase, "?z=3#frag",      "https://h.com/a/b/c.msf?z=3#frag");
   Check (sBase, "?",              "https://h.com/a/b/c.msf?");
   Check (sBase, "#",              "https://h.com/a/b/c.msf?x=1&y=2#");
   Check (sBase, "?#",             "https://h.com/a/b/c.msf?#");
   Check ("https://h.com/a?", "",  "https://h.com/a?");
   Check ("https://h.com/a?", "#", "https://h.com/a?#");

   printf ("\n--- Query and fragment ---\n");
   Check (sBase, "d.msf",          "https://h.com/a/b/d.msf");
   Check (sBase, "d.msf?k=v#f",    "https://h.com/a/b/d.msf?k=v#f");
   Check (sBase, "d.msf#f?not=query", "https://h.com/a/b/d.msf#f?not=query");
   Check (sBase, "d.msf?",         "https://h.com/a/b/d.msf?");
   Check (sBase, "d.msf#",         "https://h.com/a/b/d.msf#");
   Check (sBase, "d.msf?#",        "https://h.com/a/b/d.msf?#");

   printf ("\n--- Named placeholders ---\n");
   Check (sBase, "d.msf?<x>",          "https://h.com/a/b/d.msf?x=1");
   Check (sBase, "d.msf?<y>&<x>&k=v",  "https://h.com/a/b/d.msf?y=2&x=1&k=v");
   Check (sBase, "d.msf?<missing>&k=v","https://h.com/a/b/d.msf?k=v");
   Check (sBase, "d.msf?<missing>",    "https://h.com/a/b/d.msf");
   Check (sBase, "https://o.com/p?<x>","https://o.com/p?x=1");
   Check (sBase, "d.msf?<missing>#",   "https://h.com/a/b/d.msf#");
   Check (sBase, "https://o.com/p?<x>#","https://o.com/p?x=1#");

   printf ("\n--- Absolute and network-path references ---\n");
   Check (sBase, "http://o.com/p/../q",        "http://o.com/p/../q");
   Check (sBase, "//o.com/p/./q",              "https://o.com/p/q");
   Check (sBase, "//o.com",                    "https://o.com");
   Check (sBase, "x:foo",                      "x:foo");
   Check (sBase, "x:/p/../q",                  "x:/p/../q");
   Check (sBase, "mailto:a@b.com",             "mailto:a@b.com");
   Check (sBase, "d.msf?u=http://o.com/x",     "https://h.com/a/b/d.msf?u=http://o.com/x");
   Check (sBase, "d.msf?<x>&u=http://o.com/x", "https://h.com/a/b/d.msf?x=1&u=http://o.com/x");

   printf ("\n--- Non-hierarchical base ---\n");
   Check ("",          "d.msf",     "d.msf");
   Check ("file.msf",  "d.msf?a=1", "d.msf?a=1");
   Check ("C:/x/y",    "C:/z",      "C:/z");

   printf ("\n========================================\n");
   printf ("URL Tests: %d/%d passed\n", g_nPassed, g_nTests);
   printf ("========================================\n");

   return (g_nPassed == g_nTests) ? 0 : 1;
}
