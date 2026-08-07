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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace SNEEZE;

// ---------------------------------------------------------------------------
// Test framework
// ---------------------------------------------------------------------------

static int g_nTests  = 0;
static int g_nPassed = 0;

#define ASSERT(expr, msg) \
   do { \
      g_nTests++; \
      if (expr) { g_nPassed++; printf ("  PASS: %s\n", msg); } \
      else      { printf ("  FAIL: %s  [%s]\n", msg, #expr); } \
   } while (0)

static void BeginGroup (const char* sName)
{
   printf ("\n--- %s ---\n", sName);
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

static std::string ReadFile (const std::string& sPath)
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

// ---------------------------------------------------------------------------
// Helper: sign a raw JSON string using MSF_FILE composition path
// ---------------------------------------------------------------------------

static std::string SignPayload (const std::string& sPayload,
                                const std::string& sPrivateKey,
                                const std::vector<std::string>& aCertChain,
                                const std::string& sAlgorithm = "RS256")
{
   MSF msf;
   msf.Payload (nlohmann::json::parse (sPayload));
   for (const auto& sCert : aCertChain)
      msf.Cert_Add (sCert);
   return msf.Sign (sPrivateKey, sAlgorithm);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int RunJwsTests (int nArgc, char** aArgv)
{
   setvbuf (stdout, nullptr, _IONBF, 0);
   setvbuf (stderr, nullptr, _IONBF, 0);

#ifdef SNEEZE_TEST_CERTS_DIR
   std::string sCertsDir = SNEEZE_TEST_CERTS_DIR;
#else
   std::string sCertsDir = "tests/certs";
#endif
   if (nArgc > 1)
      sCertsDir = aArgv[1];

   printf ("JWS Integration Tests\n");
   fflush (stdout);
   printf ("Certs directory: %s\n", sCertsDir.c_str ());
   fflush (stdout);

   printf ("Loading certs...\n"); fflush (stdout);
   std::string sCaKey       = ReadFile (sCertsDir + "/ca-key.pem");
   std::string sCaCert      = ReadFile (sCertsDir + "/ca-cert.pem");
   std::string sProviderKey  = ReadFile (sCertsDir + "/provider-key.pem");
   std::string sProviderCert = ReadFile (sCertsDir + "/provider-cert.pem");
   std::string sExpiredKey   = ReadFile (sCertsDir + "/expired-key.pem");
   std::string sExpiredCert  = ReadFile (sCertsDir + "/expired-cert.pem");
   printf ("Certs loaded.\n"); fflush (stdout);

   ASSERT (!sCaKey.empty (),       "CA key loaded");
   ASSERT (!sCaCert.empty (),      "CA cert loaded");
   ASSERT (!sProviderKey.empty (), "Provider key loaded");
   ASSERT (!sProviderCert.empty (),"Provider cert loaded");

   // -----------------------------------------------------------------------
   // Test 1: Sign and verify round-trip
   // -----------------------------------------------------------------------

   BeginGroup ("Sign and Verify Round-Trip");

   std::string sPayload = R"({"namespace":"com.test.poker","organization":"Test Provider Inc."})";

   std::vector<std::string> aCertChain;
   aCertChain.push_back (sProviderCert);
   aCertChain.push_back (sCaCert);

   printf ("Signing...\n"); fflush (stdout);
   std::string sJws = SignPayload (sPayload, sProviderKey, aCertChain, "RS256");
   printf ("Sign returned (%zu bytes)\n", sJws.size ()); fflush (stdout);
   ASSERT (!sJws.empty (), "JWS signed successfully");
   ASSERT (sJws.find ('.') != std::string::npos, "JWS contains dot separators");

   MSF verifier;
   verifier.TrustedCert_Add (sCaCert);
   verifier.Parse (sJws, "test://sign-verify");
   verifier.Signature_Verify ();
   verifier.Chain_Verify ();
   ASSERT (verifier.IsSignatureValid ()  &&  verifier.IsChainTrusted (), "Verification succeeded (no error)");
   ASSERT (verifier.Algorithm () == "RS256", "Algorithm is RS256");
   ASSERT (!verifier.Fingerprint ().empty (), "Signer fingerprint computed");

   printf ("  Fingerprint: %s\n", verifier.Fingerprint ().c_str ());

   // -----------------------------------------------------------------------
   // Test 2: MSF_FILE parses MSS payload
   // -----------------------------------------------------------------------

   BeginGroup ("MSF_FILE Payload Parsing");

   std::string sMssPayload = R"({
      "Container": "poker-table",
      "Services": {
         "game-server": {
            "sNamespace": "com.test.poker",
            "sService": "websocket",
            "sConnect": "wss://rt.pokerstars.com/game",
            "sRootUrl": "https://rt.pokerstars.com",
            "bAuth": true,
            "wClass": 72,
            "twObjectIx": 0
         }
      },
      "Modules": [
         {
            "sUrl": "https://cdn.pokerstars.com/modules/game-client.wasm",
            "sHash": "sha256-a1b2c3d4e5f6"
         }
      ],
      "sSuccessor": "deadbeef0123456789abcdef"
   })";

   std::string sMssJws = SignPayload (sMssPayload, sProviderKey, aCertChain, "RS256");
   ASSERT (!sMssJws.empty (), "MSS JWS signed successfully");

   MSF svc;
   svc.TrustedCert_Add (sCaCert);
   svc.Parse (sMssJws, "test://payload-parsing");
   svc.Signature_Verify ();
   svc.Chain_Verify ();
   ASSERT (svc.IsSignatureValid ()  &&  svc.IsChainTrusted (), "MSS verification succeeded");

   ASSERT (svc.Container () == "poker-table", "Container parsed correctly");
   ASSERT (!svc.Organization ().empty (), "Organization extracted from cert");
   ASSERT (svc.Successor () == "deadbeef0123456789abcdef", "Successor parsed correctly");

   std::vector<std::string> aService_Names = svc.Service_Names ();
   ASSERT (aService_Names.size () == 1, "One service declared");
   ASSERT (svc.Service_Has ("game-server"), "Service present by name");
   ASSERT (!svc.Service_Has ("no-such-service"), "Absent service reports missing");
   nlohmann::json jService = svc.Service ("game-server");
   ASSERT (jService.value ("sService", std::string ()) == "websocket", "Service type field correct");
   ASSERT (jService.value ("sConnect", std::string ()) == "wss://rt.pokerstars.com/game", "Service connect field correct");
   ASSERT (jService.value ("sNamespace", std::string ()) == "com.test.poker", "Service namespace field correct");

   auto aModules = svc.Modules ();
   ASSERT (aModules.size () == 1, "One module declared");
   if (!aModules.empty ())
   {
      ASSERT (aModules[0].sUrl == "https://cdn.pokerstars.com/modules/game-client.wasm", "Module url correct");
      ASSERT (aModules[0].sHash == "sha256-a1b2c3d4e5f6", "Module hash correct");
   }

   // -----------------------------------------------------------------------
   // Test 3: Tampered payload rejected
   // -----------------------------------------------------------------------

   BeginGroup ("Tampered Payload Rejected");

   std::string sTampered = sJws;
   size_t nFirstDot  = sTampered.find ('.');
   size_t nSecondDot = sTampered.find ('.', nFirstDot + 1);
   if (nFirstDot != std::string::npos  &&  nSecondDot != std::string::npos)
   {
      size_t nMid = (nFirstDot + nSecondDot) / 2;
      sTampered[nMid] = (sTampered[nMid] == 'A') ? 'B' : 'A';
   }

   MSF tamperedVerifier;
   tamperedVerifier.TrustedCert_Add (sCaCert);
   tamperedVerifier.Parse (sTampered, "test://tampered");
   tamperedVerifier.Signature_Verify ();
   ASSERT (!tamperedVerifier.IsSignatureValid (), "Tampered payload rejected");
   printf ("  Error: %s\n", tamperedVerifier.SignatureError ().c_str ());

   // -----------------------------------------------------------------------
   // Test 4: Expired certificate rejected
   // -----------------------------------------------------------------------

   BeginGroup ("Expired Certificate Rejected");

   std::vector<std::string> aExpiredChain;
   aExpiredChain.push_back (sExpiredCert);
   aExpiredChain.push_back (sCaCert);

   std::string sExpiredJws = SignPayload (sPayload, sExpiredKey, aExpiredChain, "RS256");
   ASSERT (!sExpiredJws.empty (), "Expired JWS signed successfully");

   MSF expiredVerifier;
   expiredVerifier.TrustedCert_Add (sCaCert);
   expiredVerifier.Parse (sExpiredJws, "test://expired");
   expiredVerifier.Chain_Verify ();
   ASSERT (!expiredVerifier.IsChainTrusted (), "Expired certificate rejected");
   printf ("  Error: %s\n", expiredVerifier.ChainError ().c_str ());

   // -----------------------------------------------------------------------
   // Test 5: Untrusted chain rejected
   // -----------------------------------------------------------------------

   BeginGroup ("Untrusted Chain Rejected");

   MSF untrustedVerifier;
   untrustedVerifier.Parse (sJws, "test://untrusted");
   untrustedVerifier.Chain_Verify ();
   ASSERT (!untrustedVerifier.IsChainTrusted (), "Untrusted chain rejected");
   printf ("  Error: %s\n", untrustedVerifier.ChainError ().c_str ());

   // -----------------------------------------------------------------------
   // Test 6: Fingerprint stability
   // -----------------------------------------------------------------------

   BeginGroup ("Fingerprint Stability");

   MSF verifier2;
   verifier2.TrustedCert_Add (sCaCert);
   verifier2.Parse (sJws, "test://sign-verify");
   verifier2.Signature_Verify ();
   verifier2.Chain_Verify ();
   ASSERT (verifier2.IsSignatureValid ()  &&  verifier2.IsChainTrusted (), "Second verification succeeded");
   ASSERT (verifier.Fingerprint () == verifier2.Fingerprint (), "Fingerprint is stable across verifications");

   // -----------------------------------------------------------------------
   // Test 7: Malformed JWS rejected
   // -----------------------------------------------------------------------

   BeginGroup ("Malformed JWS Rejected");

   MSF malformedVerifier;

   bool bEmpty = malformedVerifier.Parse ("", "test://malformed");
   ASSERT (!bEmpty, "Empty string rejected");

   bool bGarbage = malformedVerifier.Parse ("not.a.jws", "test://malformed");
   ASSERT (!bGarbage, "Garbage string rejected");

   bool bPartial = malformedVerifier.Parse ("eyJhbGciOiJSUzI1NiJ9", "test://malformed");
   ASSERT (!bPartial, "Partial JWS (one segment) rejected");

   // -----------------------------------------------------------------------
   // Test 8: Parse always populates data (even without verification)
   // -----------------------------------------------------------------------

   BeginGroup ("Parse Populates Data Without Verification");

   MSF parseOnly;
   bool bParsed = parseOnly.Parse (sMssJws, "test://parse-only");
   ASSERT (bParsed, "Parse succeeded without verification");
   ASSERT (parseOnly.Container () == "poker-table", "Container available without verify");
   ASSERT (!parseOnly.Fingerprint ().empty (), "Fingerprint available without verify");
   ASSERT (parseOnly.Cert_Count () == 2, "Cert count available without verify");

   // -----------------------------------------------------------------------
   // Test 9: Composition round-trip
   // -----------------------------------------------------------------------

   BeginGroup ("Composition Round-Trip");

   MSF composer;
   composer.Container ("my-container");
   composer.Service_Add ("my-svc", nlohmann::json {{"sService", "grpc"}, {"sConnect", "grpc://example.com:443"}});
   composer.Module_Add ("https://example.com/mod.wasm", "sha256-abcdef123456");
   composer.Cert_Add (sProviderCert);
   composer.Cert_Add (sCaCert);

   std::string sComposedJws = composer.Sign (sProviderKey, "RS256");
   ASSERT (!sComposedJws.empty (), "Composed MSF signed successfully");

   MSF reader;
   reader.TrustedCert_Add (sCaCert);
   reader.Parse (sComposedJws, "test://composition");
   reader.Signature_Verify ();
   reader.Chain_Verify ();
   ASSERT (reader.IsSignatureValid ()  &&  reader.IsChainTrusted (), "Composed MSF verifies");
   ASSERT (reader.Container () == "my-container", "Composed container round-trips");
   ASSERT (!reader.Organization ().empty (), "Composed organization from cert");
   ASSERT (reader.Service_Names ().size () == 1, "Composed service round-trips");
   ASSERT (reader.Service_Has ("my-svc"), "Composed service present by name");
   ASSERT (reader.Service ("my-svc").value ("sConnect", std::string ()) == "grpc://example.com:443", "Composed service connect round-trips");

   auto aComposedModules = reader.Modules ();
   ASSERT (aComposedModules.size () == 1, "Composed module round-trips");
   if (!aComposedModules.empty ())
   {
      ASSERT (aComposedModules[0].sUrl == "https://example.com/mod.wasm", "Composed module url round-trips");
      ASSERT (aComposedModules[0].sHash == "sha256-abcdef123456", "Composed module hash round-trips");
   }

   // -----------------------------------------------------------------------
   // Summary
   // -----------------------------------------------------------------------

   printf ("\n========================================\n");
   printf ("JWS Tests: %d/%d passed\n", g_nPassed, g_nTests);
   printf ("========================================\n");

   return (g_nPassed == g_nTests) ? 0 : 1;
}
