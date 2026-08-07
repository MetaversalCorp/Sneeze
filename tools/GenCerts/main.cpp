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

// One-shot utility: generates test CA + provider certificates for JWS tests.
// Compile against BoringSSL, run once, check the resulting PEM files into
// tests/certs/.  Invoke manually when the certificate fixtures need to be
// refreshed; it is not run as part of CTest.

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

static bool WriteFile (const char* sPath, const char* sData)
{
   FILE* pFile = fopen (sPath, "wb");
   bool bOk = false;
   if (pFile)
   {
      fwrite (sData, 1, strlen (sData), pFile);
      fclose (pFile);
      bOk = true;
   }
   return bOk;
}

static char* BioToString (BIO* pBio)
{
   char* pData = nullptr;
   long nLen = BIO_get_mem_data (pBio, &pData);
   char* sResult = (char*) malloc (nLen + 1);
   memcpy (sResult, pData, nLen);
   sResult[nLen] = '\0';
   return sResult;
}

static EVP_PKEY* GenerateRsaKey (int nBits)
{
   EVP_PKEY* pKey = nullptr;
   EVP_PKEY_CTX* pCtx = EVP_PKEY_CTX_new_id (EVP_PKEY_RSA, nullptr);
   if (pCtx)
   {
      EVP_PKEY_keygen_init (pCtx);
      EVP_PKEY_CTX_set_rsa_keygen_bits (pCtx, nBits);
      EVP_PKEY_keygen (pCtx, &pKey);
      EVP_PKEY_CTX_free (pCtx);
   }
   return pKey;
}

static void AddExtension (X509* pCert, int nNid, const char* sValue)
{
   X509V3_CTX ctx;
   X509V3_set_ctx_nodb (&ctx);
   X509V3_set_ctx (&ctx, pCert, pCert, nullptr, nullptr, 0);
   X509_EXTENSION* pExt = X509V3_EXT_nconf_nid (nullptr, &ctx, nNid, sValue);
   if (pExt)
   {
      X509_add_ext (pCert, pExt, -1);
      X509_EXTENSION_free (pExt);
   }
}

static X509* CreateCaCert (EVP_PKEY* pCaKey)
{
   X509* pCert = X509_new ();

   X509_set_version (pCert, 2); // X.509 v3 (zero-indexed: 2 -> v3)
   ASN1_INTEGER_set (X509_get_serialNumber (pCert), 1);
   X509_gmtime_adj (X509_get_notBefore (pCert), 0);
   X509_gmtime_adj (X509_get_notAfter (pCert), 365 * 24 * 3600);

   X509_NAME* pName = X509_get_subject_name (pCert);
   X509_NAME_add_entry_by_txt (pName, "C",  MBSTRING_ASC, (const unsigned char*) "US", -1, -1, 0);
   X509_NAME_add_entry_by_txt (pName, "O",  MBSTRING_ASC, (const unsigned char*) "OMBI Test CA", -1, -1, 0);
   X509_NAME_add_entry_by_txt (pName, "CN", MBSTRING_ASC, (const unsigned char*) "OMBI Test Root CA", -1, -1, 0);
   X509_set_issuer_name (pCert, pName);

   X509_set_pubkey (pCert, pCaKey);

   AddExtension (pCert, NID_basic_constraints, "critical,CA:TRUE");
   AddExtension (pCert, NID_key_usage, "critical,keyCertSign,cRLSign");
   AddExtension (pCert, NID_subject_key_identifier, "hash");

   X509_sign (pCert, pCaKey, EVP_sha256 ());
   return pCert;
}

static X509* CreateProviderCert (EVP_PKEY* pProviderKey, X509* pCaCert, EVP_PKEY* pCaKey, long nSerial, long nValiditySecs)
{
   X509* pCert = X509_new ();

   X509_set_version (pCert, 2); // X.509 v3 (zero-indexed: 2 -> v3)
   ASN1_INTEGER_set (X509_get_serialNumber (pCert), nSerial);
   X509_gmtime_adj (X509_get_notBefore (pCert), 0);
   X509_gmtime_adj (X509_get_notAfter (pCert), nValiditySecs);

   X509_NAME* pSubject = X509_get_subject_name (pCert);
   X509_NAME_add_entry_by_txt (pSubject, "C",  MBSTRING_ASC, (const unsigned char*) "US", -1, -1, 0);
   X509_NAME_add_entry_by_txt (pSubject, "O",  MBSTRING_ASC, (const unsigned char*) "Test Provider Inc.", -1, -1, 0);
   X509_NAME_add_entry_by_txt (pSubject, "CN", MBSTRING_ASC, (const unsigned char*) "Test Provider", -1, -1, 0);
   X509_set_issuer_name (pCert, X509_get_subject_name (pCaCert));

   X509_set_pubkey (pCert, pProviderKey);

   X509V3_CTX ctx;
   X509V3_set_ctx_nodb (&ctx);
   X509V3_set_ctx (&ctx, pCaCert, pCert, nullptr, nullptr, 0);

   X509_EXTENSION* pExt = X509V3_EXT_nconf_nid (nullptr, &ctx, NID_basic_constraints, "critical,CA:FALSE");
   if (pExt) { X509_add_ext (pCert, pExt, -1); X509_EXTENSION_free (pExt); }

   pExt = X509V3_EXT_nconf_nid (nullptr, &ctx, NID_key_usage, "critical,digitalSignature");
   if (pExt) { X509_add_ext (pCert, pExt, -1); X509_EXTENSION_free (pExt); }

   pExt = X509V3_EXT_nconf_nid (nullptr, &ctx, NID_subject_key_identifier, "hash");
   if (pExt) { X509_add_ext (pCert, pExt, -1); X509_EXTENSION_free (pExt); }

   pExt = X509V3_EXT_nconf_nid (nullptr, &ctx, NID_authority_key_identifier, "keyid:always");
   if (pExt) { X509_add_ext (pCert, pExt, -1); X509_EXTENSION_free (pExt); }

   X509_sign (pCert, pCaKey, EVP_sha256 ());
   return pCert;
}

static char* KeyToPem (EVP_PKEY* pKey)
{
   BIO* pBio = BIO_new (BIO_s_mem ());
   PEM_write_bio_PrivateKey (pBio, pKey, nullptr, nullptr, 0, nullptr, nullptr);
   char* sResult = BioToString (pBio);
   BIO_free (pBio);
   return sResult;
}

static char* CertToPem (X509* pCert)
{
   BIO* pBio = BIO_new (BIO_s_mem ());
   PEM_write_bio_X509 (pBio, pCert);
   char* sResult = BioToString (pBio);
   BIO_free (pBio);
   return sResult;
}

int main (int nArgc, char** aArgv)
{
   const char* sOutDir = "tests/certs";
   if (nArgc > 1)
      sOutDir = aArgv[1];

   printf ("Generating test certificates in %s/\n", sOutDir);

   EVP_PKEY* pCaKey       = GenerateRsaKey (2048);
   EVP_PKEY* pProviderKey = GenerateRsaKey (2048);

   X509* pCaCert       = CreateCaCert (pCaKey);
   X509* pProviderCert = CreateProviderCert (pProviderKey, pCaCert, pCaKey, 2, 365 * 24 * 3600);

   // Expired cert (validity = -1 second, so notAfter is in the past)
   EVP_PKEY* pExpiredKey  = GenerateRsaKey (2048);
   X509* pExpiredCert     = CreateProviderCert (pExpiredKey, pCaCert, pCaKey, 3, -1);

   char sPath[512];

   snprintf (sPath, sizeof (sPath), "%s/ca-key.pem", sOutDir);
   char* sPem = KeyToPem (pCaKey);
   WriteFile (sPath, sPem);
   free (sPem);
   printf ("  %s\n", sPath);

   snprintf (sPath, sizeof (sPath), "%s/ca-cert.pem", sOutDir);
   sPem = CertToPem (pCaCert);
   WriteFile (sPath, sPem);
   free (sPem);
   printf ("  %s\n", sPath);

   snprintf (sPath, sizeof (sPath), "%s/provider-key.pem", sOutDir);
   sPem = KeyToPem (pProviderKey);
   WriteFile (sPath, sPem);
   free (sPem);
   printf ("  %s\n", sPath);

   snprintf (sPath, sizeof (sPath), "%s/provider-cert.pem", sOutDir);
   sPem = CertToPem (pProviderCert);
   WriteFile (sPath, sPem);
   free (sPem);
   printf ("  %s\n", sPath);

   snprintf (sPath, sizeof (sPath), "%s/expired-key.pem", sOutDir);
   sPem = KeyToPem (pExpiredKey);
   WriteFile (sPath, sPem);
   free (sPem);
   printf ("  %s\n", sPath);

   snprintf (sPath, sizeof (sPath), "%s/expired-cert.pem", sOutDir);
   sPem = CertToPem (pExpiredCert);
   WriteFile (sPath, sPem);
   free (sPem);
   printf ("  %s\n", sPath);

   X509_free (pExpiredCert);
   EVP_PKEY_free (pExpiredKey);
   X509_free (pProviderCert);
   X509_free (pCaCert);
   EVP_PKEY_free (pProviderKey);
   EVP_PKEY_free (pCaKey);

   printf ("Done.\n");
   return 0;
}
