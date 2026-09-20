#!/usr/bin/env python3
# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Generates the C++ translation unit that embeds Mozilla's CA bundle, which
# BoringSSL needs because it ships no trust store of its own. Both build systems
# call this one script: src/CMakeLists.txt at configure time, and the
# hand-maintained msvc/Sneeze.vcxproj from a pre-build target.
#
#   python tools/GenCaCert/gencacert.py --pem <cacert.pem> --out <cacert_data.cpp>
#
# The bundle is downloaded to --pem on first use and reused afterwards, so an
# offline build works as long as that file survives.

import argparse
import os
import sys
import urllib.request

CACERT_URL = "https://curl.se/ca/cacert.pem"

# MSVC caps a single string literal at 65535 bytes, so the bundle is emitted as
# adjacent literals the compiler concatenates.
CHUNK_SIZE = 8000


def Download (sPathname):
   print ("Fetching cacert.pem from curl.se")
   with urllib.request.urlopen (CACERT_URL, timeout = 30) as pResponse:
      aByte = pResponse.read ()
   if len (aByte) < 100000:
      raise RuntimeError ("cacert.pem download looks truncated ({} bytes)".format (len (aByte)))
   os.makedirs (os.path.dirname (os.path.abspath (sPathname)), exist_ok = True)
   with open (sPathname, "wb") as pFile:
      pFile.write (aByte)
   return aByte.decode ("utf-8")


def Escape (sChunk):
   sChunk = sChunk.replace ("\\", "\\\\")
   sChunk = sChunk.replace ("\"", "\\\"")
   sChunk = sChunk.replace ("\n", "\\n\"\n      \"")
   return sChunk


def Generate (sPem, sPathname_Out):
   aLine = []
   aLine.append ("// Auto-generated - do not edit. Mozilla NSS CA bundle, embedded for the\n")
   aLine.append ("// BoringSSL-backed fetch and socket paths (curl CURLOPT_CAINFO_BLOB and the\n")
   aLine.append ("// WebSocket TLS context). Regenerate with tools/GenCaCert/gencacert.py.\n")
   aLine.append ("namespace SNEEZE {\n")
   aLine.append ("   extern const char* const g_szCaCertPem;\n")
   aLine.append ("   extern const unsigned long g_nCaCertPemLen;\n")
   aLine.append ("}\n")
   aLine.append ("namespace SNEEZE {\n")
   aLine.append ("   static const char s_data[] =\n")
   for nOffset in range (0, len (sPem), CHUNK_SIZE):
      aLine.append ("      \"{}\"\n".format (Escape (sPem[nOffset : nOffset + CHUNK_SIZE])))
   aLine.append ("      ;\n")
   aLine.append ("   const char* const g_szCaCertPem = s_data;\n")
   aLine.append ("   const unsigned long g_nCaCertPemLen = sizeof(s_data) - 1;\n")
   aLine.append ("}\n")

   sText = "".join (aLine)
   os.makedirs (os.path.dirname (os.path.abspath (sPathname_Out)), exist_ok = True)

   # Rewriting an unchanged file would make every consumer rebuild.
   sText_Old = None
   if os.path.exists (sPathname_Out):
      with open (sPathname_Out, "r", encoding = "utf-8", newline = "") as pFile:
         sText_Old = pFile.read ()
   if sText_Old != sText:
      with open (sPathname_Out, "w", encoding = "utf-8", newline = "") as pFile:
         pFile.write (sText)


def Main ():
   pParser = argparse.ArgumentParser (description = "Embed the Mozilla CA bundle as a C++ source file.")
   pParser.add_argument ("--pem", required = True, help = "cacert.pem path (downloaded if absent)")
   pParser.add_argument ("--out", required = True, help = "generated .cpp path")
   pArgs = pParser.parse_args ()

   if os.path.exists (pArgs.pem):
      with open (pArgs.pem, "r", encoding = "utf-8", newline = "") as pFile:
         sPem = pFile.read ()
   else:
      sPem = Download (pArgs.pem)

   Generate (sPem, pArgs.out)
   return 0


if __name__ == "__main__":
   sys.exit (Main ())
