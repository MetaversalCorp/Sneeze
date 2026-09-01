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

// SignMsf - JWS signing and verification CLI tool for .msf files.
//
// Sign:
//   SignMsf --payload <json> --key <key.pem> --cert <cert.pem>
//           [--chain <intermediate.pem>] [--alg RS256] --out <output.msf>
//
// Verify / dump:
//   SignMsf --verify <file.msf> [--trust <ca.pem>]

#include <Sneeze.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace SNEEZE;

static std::string ReadFile (const char* sPath)
{
   std::string sResult;
   std::ifstream ifs (sPath, std::ios::binary);
   if (ifs.is_open ())
   {
      std::ostringstream oss;
      oss << ifs.rdbuf ();
      sResult = oss.str ();
   }
   return sResult;
}

static bool WriteFile (const char* sPath, const std::string& sData)
{
   bool bOk = false;
   std::ofstream ofs (sPath, std::ios::binary);
   if (ofs.is_open ())
   {
      ofs.write (sData.data (), (std::streamsize) sData.size ());
      bOk = ofs.good ();
   }
   return bOk;
}

static void PrintUsage ()
{
   std::cerr
      << "Usage:\n"
      << "  Sign:    SignMsf --payload <json> --key <key.pem> --cert <cert.pem>\n"
      << "                   [--chain <intermediate.pem>] [--alg RS256] --out <file.msf>\n"
      << "\n"
      << "  Verify:  SignMsf --verify <file.msf> [--trust <ca.pem>]\n";
}

// ---------------------------------------------------------------------------
// Verify mode
// ---------------------------------------------------------------------------

static void PrintCertChain (const MSF& msf)
{
   const auto& aCerts = msf.Certs ();

   for (size_t i = 0; i < aCerts.size (); ++i)
   {
      const auto& cert = aCerts[i];
      const char* sLabel = cert.bIsCA ? "CA" : "Leaf";

      std::cout << "\n";
      std::cout << "  Certificate [" << i << "] (" << sLabel << ")\n";
      std::cout << "    Subject:    " << cert.sSubject << "\n";
      std::cout << "    Issuer:     " << cert.sIssuer << "\n";
      std::cout << "    Serial:     " << cert.sSerial << "\n";
      std::cout << "    Not Before: " << cert.sNotBefore << "\n";
      std::cout << "    Not After:  " << cert.sNotAfter << "\n";
      std::cout << "    Key:        " << cert.sKeyType << " " << cert.nKeyBits << "-bit\n";
   }
}

static int DoVerify (const char* sMsfPath, const std::vector<const char*>& aTrustPaths)
{
   int nResult = 1;

   std::string sJws = ReadFile (sMsfPath);
   if (sJws.empty ())
   {
      std::cerr << "Error: cannot read file: " << sMsfPath << "\n";
   }
   else
   {
      MSF msf;

      for (const char* sTrustPath : aTrustPaths)
      {
         std::string sPem = ReadFile (sTrustPath);
         if (!sPem.empty ())
            msf.TrustedCert_Add (sPem);
         else
            std::cerr << "Warning: cannot read trust cert: " << sTrustPath << "\n";
      }

      if (!msf.Parse (sJws, sMsfPath))
      {
         std::cerr << "Error: failed to parse JWS from " << sMsfPath << "\n";
      }
      else
      {
         msf.Signature_Verify ();
         msf.Chain_Verify ();

         std::cout << "File:        " << sMsfPath << "\n";
         std::cout << "Algorithm:   " << msf.Algorithm () << "\n";
         std::cout << "Fingerprint: " << msf.Fingerprint () << "\n";

         std::string sSuccessor = msf.Successor ();
         if (!sSuccessor.empty ())
            std::cout << "Successor:   " << sSuccessor << "\n";

         if (msf.IsSignatureValid ()  &&  msf.IsChainTrusted ())
         {
            std::cout << "Signature:   VERIFIED\n";
            nResult = 0;
         }
         else
         {
            std::cout << "Signature:   FAILED\n";
            if (!msf.SignatureError ().empty ())
               std::cerr << "Sig error:   " << msf.SignatureError () << "\n";
            if (!msf.ChainError ().empty ())
               std::cerr << "Chain error: " << msf.ChainError () << "\n";
         }

         std::cout << "\n--- Certificate Chain ---";
         PrintCertChain (msf);

         std::cout << "\n--- Payload ---\n\n";
         nlohmann::json payload = msf.Payload ();
         if (!payload.is_null ())
            std::cout << payload.dump (3) << "\n";
         else
            std::cout << "(empty)\n";
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// Sign mode
// ---------------------------------------------------------------------------

static int DoSign (const char* sPayloadPath, const char* sKeyPath,
                   const std::vector<const char*>& aCertPaths,
                   const std::string& sAlgorithm, const char* sOutPath)
{
   int nResult = 1;

   std::string sPayload = ReadFile (sPayloadPath);
   std::string sKey     = ReadFile (sKeyPath);

   if (sPayload.empty ())
      std::cerr << "Error: cannot read payload file: " << sPayloadPath << "\n";
   else if (sKey.empty ())
      std::cerr << "Error: cannot read key file: " << sKeyPath << "\n";
   else
   {
      MSF msf;

      msf.Payload (nlohmann::json::parse (sPayload));

      bool bCertsOk = true;
      for (const char* sPath : aCertPaths)
      {
         std::string sCert = ReadFile (sPath);
         if (sCert.empty ())
         {
            std::cerr << "Error: cannot read certificate file: " << sPath << "\n";
            bCertsOk = false;
            break;
         }
         msf.Cert_Add (sCert);
      }

      if (bCertsOk)
      {
         std::string sJws = msf.Sign (sKey, sAlgorithm);
         if (sJws.empty ())
         {
            std::cerr << "Error: signing failed\n";
         }
         else if (!WriteFile (sOutPath, sJws))
         {
            std::cerr << "Error: cannot write output file: " << sOutPath << "\n";
         }
         else
         {
            std::cout << "Signed " << sOutPath << " (" << sJws.size () << " bytes)\n";
            nResult = 0;
         }
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main (int nArgc, char** aArgv)
{
   const char* sPayloadPath = nullptr;
   const char* sKeyPath     = nullptr;
   const char* sOutPath     = nullptr;
   const char* sVerifyPath  = nullptr;
   std::string sAlgorithm   = "RS256";
   std::vector<const char*> aCertPaths;
   std::vector<const char*> aTrustPaths;
   bool bBadArgs = false;

   for (int i = 1; i < nArgc  &&  !bBadArgs; ++i)
   {
      if (strcmp (aArgv[i], "--verify") == 0  &&  i + 1 < nArgc)
         sVerifyPath = aArgv[++i];
      else if (strcmp (aArgv[i], "--trust") == 0  &&  i + 1 < nArgc)
         aTrustPaths.push_back (aArgv[++i]);
      else if (strcmp (aArgv[i], "--payload") == 0  &&  i + 1 < nArgc)
         sPayloadPath = aArgv[++i];
      else if (strcmp (aArgv[i], "--key") == 0  &&  i + 1 < nArgc)
         sKeyPath = aArgv[++i];
      else if (strcmp (aArgv[i], "--cert") == 0  &&  i + 1 < nArgc)
         aCertPaths.push_back (aArgv[++i]);
      else if (strcmp (aArgv[i], "--chain") == 0  &&  i + 1 < nArgc)
         aCertPaths.push_back (aArgv[++i]);
      else if (strcmp (aArgv[i], "--alg") == 0  &&  i + 1 < nArgc)
         sAlgorithm = aArgv[++i];
      else if (strcmp (aArgv[i], "--out") == 0  &&  i + 1 < nArgc)
         sOutPath = aArgv[++i];
      else
      {
         std::cerr << "Unknown argument: " << aArgv[i] << "\n";
         bBadArgs = true;
      }
   }

   int nResult = 1;

   if (bBadArgs)
      PrintUsage ();
   else if (sVerifyPath)
      nResult = DoVerify (sVerifyPath, aTrustPaths);
   else if (!sPayloadPath  ||  !sKeyPath  ||  aCertPaths.empty ()  ||  !sOutPath)
      PrintUsage ();
   else
      nResult = DoSign (sPayloadPath, sKeyPath, aCertPaths, sAlgorithm, sOutPath);

   return nResult;
}
