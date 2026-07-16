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

using namespace SNEEZE;

using CHAIN    = MSF::CHAIN;
using MSF_CERT = MSF::CERT;

struct CHAIN::IMPL
{
   X509_STORE* pStore;
   X509*       pLeaf;

   IMPL () : pStore (nullptr), pLeaf (nullptr) {}

   ~IMPL ()
   {
      if (pLeaf)
         X509_free (pLeaf);
      if (pStore)
         X509_STORE_free (pStore);
   }
};

CHAIN::CHAIN ()
   : m_pImpl (new IMPL)
{
}

CHAIN::~CHAIN ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// LoadTrustStore
// ---------------------------------------------------------------------------

void CHAIN::LoadTrustStore ()
{
   if (!m_pImpl->pStore)
   {
      m_pImpl->pStore = X509_STORE_new ();

#ifdef _WIN32
      HCERTSTORE hSysStore = CertOpenSystemStoreA (0, "ROOT");
      if (hSysStore)
      {
         PCCERT_CONTEXT pCtx = nullptr;
         while ((pCtx = CertEnumCertificatesInStore (hSysStore, pCtx)) != nullptr)
         {
            const unsigned char* pDer = pCtx->pbCertEncoded;
            X509* pCert = d2i_X509 (nullptr, &pDer, (long) pCtx->cbCertEncoded);
            if (pCert)
            {
               X509_STORE_add_cert (m_pImpl->pStore, pCert);
               X509_free (pCert);
            }
         }
         CertCloseStore (hSysStore, 0);
      }
#else
      X509_STORE_set_default_paths (m_pImpl->pStore);
#endif
   }
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

static X509* DecodeDerBase64 (const std::string& sB64)
{
   size_t nMaxLen = (sB64.size () / 4 + 1) * 3;
   std::vector<unsigned char> aDer (nMaxLen);

   int nLen = EVP_DecodeBlock (aDer.data (),
      (const unsigned char*) sB64.data (), (int) sB64.size ());
   if (nLen <= 0)
      return nullptr;

   size_t nPad = 0;
   for (auto it = sB64.rbegin (); it != sB64.rend ()  &&  *it == '='; ++it)
      nPad++;
   nLen -= (int) nPad;

   const unsigned char* pDer = aDer.data ();
   return d2i_X509 (nullptr, &pDer, nLen);
}

// ---------------------------------------------------------------------------
// Cert info extraction helpers
// ---------------------------------------------------------------------------

static std::string X509NameOneLine (X509_NAME* pName)
{
   std::string sResult;
   if (!pName)
      return sResult;

   BIO* pBio = BIO_new (BIO_s_mem ());
   X509_NAME_print_ex (pBio, pName, 0, XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB);
   char* pData = nullptr;
   long nLen = BIO_get_mem_data (pBio, &pData);
   if (nLen > 0)
      sResult.assign (pData, (size_t) nLen);
   BIO_free (pBio);
   return sResult;
}

static std::string X509NameField (X509_NAME* pName, int nNid)
{
   std::string sResult;
   if (!pName)
      return sResult;

   int nIndex = X509_NAME_get_index_by_NID (pName, nNid, -1);
   if (nIndex >= 0)
   {
      X509_NAME_ENTRY* pEntry = X509_NAME_get_entry (pName, nIndex);
      if (pEntry)
      {
         ASN1_STRING* pData = X509_NAME_ENTRY_get_data (pEntry);
         if (pData)
         {
            const unsigned char* pUtf8 = ASN1_STRING_get0_data (pData);
            int nLen = ASN1_STRING_length (pData);
            if (pUtf8  &&  nLen > 0)
               sResult.assign ((const char*) pUtf8, (size_t) nLen);
         }
      }
   }

   return sResult;
}

static std::string Asn1TimeToString (const ASN1_TIME* pTime)
{
   std::string sResult;
   if (!pTime)
      return sResult;

   BIO* pBio = BIO_new (BIO_s_mem ());
   ASN1_TIME_print (pBio, pTime);
   char* pData = nullptr;
   long nLen = BIO_get_mem_data (pBio, &pData);
   if (nLen > 0)
      sResult.assign (pData, (size_t) nLen);
   BIO_free (pBio);
   return sResult;
}

static std::string SerialToHex (X509* pCert)
{
   const ASN1_INTEGER* pSerial = X509_get_serialNumber (pCert);
   if (!pSerial)
      return "(none)";

   BIGNUM* pBn = ASN1_INTEGER_to_BN (pSerial, nullptr);
   if (!pBn)
      return "(none)";

   char* pHex = BN_bn2hex (pBn);
   BN_free (pBn);

   std::string sResult;
   if (pHex)
   {
      sResult = pHex;
      OPENSSL_free (pHex);
   }
   return sResult;
}

static MSF_CERT ExtractCertInfo (X509* pCert, bool bIsCA)
{
   MSF_CERT info;

   info.sSubject      = X509NameOneLine (X509_get_subject_name (pCert));
   info.sIssuer       = X509NameOneLine (X509_get_issuer_name (pCert));
   info.sOrganization = X509NameField (X509_get_subject_name (pCert), NID_organizationName);
   info.sSerial       = SerialToHex (pCert);
   info.sNotBefore    = Asn1TimeToString (X509_get_notBefore (pCert));
   info.sNotAfter     = Asn1TimeToString (X509_get_notAfter (pCert));
   info.bIsCA         = bIsCA;

   EVP_PKEY* pKey = X509_get_pubkey (pCert);
   if (pKey)
   {
      int nType = EVP_PKEY_id (pKey);
      info.nKeyBits = EVP_PKEY_bits (pKey);

      if (nType == EVP_PKEY_RSA)
         info.sKeyType = "RSA";
      else if (nType == EVP_PKEY_EC)
         info.sKeyType = "EC";
      else
         info.sKeyType = "unknown";

      EVP_PKEY_free (pKey);
   }
   else
   {
      info.sKeyType = "unknown";
      info.nKeyBits = 0;
   }

   return info;
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

bool CHAIN::Validate (const std::vector<std::string>& aX5cEntries, std::string& sError)
{
   m_aCertInfos.clear ();

   if (aX5cEntries.empty ())
   {
      sError = "x5c chain is empty";
      return false;
   }

   LoadTrustStore ();

   std::vector<X509*> aCerts;
   for (const auto& sEntry : aX5cEntries)
   {
      X509* pCert = DecodeDerBase64 (sEntry);
      if (!pCert)
      {
         sError = "failed to decode x5c certificate";
         for (auto* p : aCerts)
            X509_free (p);
         return false;
      }
      aCerts.push_back (pCert);
   }

   for (size_t i = 0; i < aCerts.size (); ++i)
      m_aCertInfos.push_back (ExtractCertInfo (aCerts[i], i > 0));

   X509* pLeaf = aCerts[0];

   STACK_OF (X509)* pIntermediates = sk_X509_new_null ();
   for (size_t i = 1; i < aCerts.size (); ++i)
      sk_X509_push (pIntermediates, aCerts[i]);

   X509_STORE_CTX* pCtx = X509_STORE_CTX_new ();
   X509_STORE_CTX_init (pCtx, m_pImpl->pStore, pLeaf, pIntermediates);

   bool bResult = false;
   int nRc = X509_verify_cert (pCtx);
   if (nRc == 1)
   {
      bResult = true;

      if (m_pImpl->pLeaf)
         X509_free (m_pImpl->pLeaf);
      m_pImpl->pLeaf = X509_dup (pLeaf);
   }
   else
   {
      int nErr = X509_STORE_CTX_get_error (pCtx);
      sError = X509_verify_cert_error_string (nErr);
   }

   X509_STORE_CTX_free (pCtx);
   sk_X509_free (pIntermediates);

   for (auto* p : aCerts)
      X509_free (p);

   return bResult;
}

// ---------------------------------------------------------------------------
// Internal fingerprint helper (operates on an X509*)
// ---------------------------------------------------------------------------

static std::string ComputeSpkiFingerprint (X509* pCert)
{
   std::string sResult;

   if (pCert)
   {
      unsigned char aPubKeyDer[8192];
      unsigned char* pOut = aPubKeyDer;
      int nLen = i2d_PUBKEY (X509_get_pubkey (pCert), &pOut);

      if (nLen > 0)
      {
         unsigned char aDigest[SHA256_DIGEST_LENGTH];
         SHA256 (aPubKeyDer, (size_t) nLen, aDigest);

         std::ostringstream oss;
         oss << std::hex << std::setfill ('0');
         for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            oss << std::setw (2) << (int) aDigest[i];

         sResult = oss.str ();
      }
   }

   return sResult;
}

// ---------------------------------------------------------------------------
// GetLeafFingerprint
// ---------------------------------------------------------------------------

std::string CHAIN::GetLeafFingerprint () const
{
   return ComputeSpkiFingerprint (m_pImpl->pLeaf);
}

// ---------------------------------------------------------------------------
// CertInfos
// ---------------------------------------------------------------------------

const std::vector<MSF_CERT>& CHAIN::CertInfos () const
{
   return m_aCertInfos;
}

// ---------------------------------------------------------------------------
// AddTrustedCert
// ---------------------------------------------------------------------------

void CHAIN::AddTrustedCert (const std::string& sPem)
{
   LoadTrustStore ();

   BIO* pBio = BIO_new_mem_buf (sPem.data (), (int) sPem.size ());
   X509* pCert = PEM_read_bio_X509 (pBio, nullptr, nullptr, nullptr);
   BIO_free (pBio);

   if (pCert)
   {
      X509_STORE_add_cert (m_pImpl->pStore, pCert);
      X509_free (pCert);
   }
}

// ---------------------------------------------------------------------------
// Static cert utilities
// ---------------------------------------------------------------------------

MSF_CERT CHAIN::DecodeInfoDerBase64 (const std::string& sB64, bool bIsCA)
{
   MSF_CERT info;
   X509* pCert = DecodeDerBase64 (sB64);
   if (pCert)
   {
      info = ExtractCertInfo (pCert, bIsCA);
      X509_free (pCert);
   }
   return info;
}

MSF_CERT CHAIN::DecodeInfoPem (const std::string& sPem, bool bIsCA)
{
   MSF_CERT info;
   BIO* pBio = BIO_new_mem_buf (sPem.data (), (int) sPem.size ());
   X509* pCert = PEM_read_bio_X509 (pBio, nullptr, nullptr, nullptr);
   BIO_free (pBio);

   if (pCert)
   {
      info = ExtractCertInfo (pCert, bIsCA);
      X509_free (pCert);
   }
   return info;
}

std::string CHAIN::ComputeFingerprint (const std::string& sB64Der)
{
   std::string sResult;
   X509* pCert = DecodeDerBase64 (sB64Der);
   if (pCert)
   {
      sResult = ComputeSpkiFingerprint (pCert);
      X509_free (pCert);
   }
   return sResult;
}

std::string CHAIN::ExtractPublicKeyPem (const std::string& sB64Der)
{
   std::string sResult;
   X509* pCert = DecodeDerBase64 (sB64Der);
   if (pCert)
   {
      EVP_PKEY* pPubKey = X509_get_pubkey (pCert);
      X509_free (pCert);

      if (pPubKey)
      {
         BIO* pBio = BIO_new (BIO_s_mem ());
         PEM_write_bio_PUBKEY (pBio, pPubKey);
         EVP_PKEY_free (pPubKey);

         char* pData = nullptr;
         long nLen = BIO_get_mem_data (pBio, &pData);
         if (nLen > 0)
            sResult.assign (pData, (size_t) nLen);
         BIO_free (pBio);
      }
   }
   return sResult;
}

std::string CHAIN::PemToDerBase64 (const std::string& sPem)
{
   std::string sResult;

   BIO* pBio = BIO_new_mem_buf (sPem.data (), (int) sPem.size ());
   ERR_clear_error ();
   X509* pCert = PEM_read_bio_X509 (pBio, nullptr, nullptr, nullptr);
   BIO_free (pBio);

   if (pCert)
   {
      unsigned char* pDer = nullptr;
      int nLen = i2d_X509 (pCert, &pDer);
      X509_free (pCert);

      if (nLen > 0  &&  pDer)
      {
         size_t nB64Len = ((size_t) nLen + 2) / 3 * 4;
         std::vector<unsigned char> aB64 (nB64Len + 1);
         EVP_EncodeBlock (aB64.data (), pDer, nLen);
         sResult.assign ((const char*) aB64.data ());
         OPENSSL_free (pDer);
      }
   }

   return sResult;
}

std::string CHAIN::HashString (const std::string& sInput)
{
   std::string sResult;

   if (!sInput.empty ())
   {
      unsigned char aDigest[SHA256_DIGEST_LENGTH];
      SHA256 ((const unsigned char*) sInput.data (), sInput.size (), aDigest);

      std::ostringstream oss;
      oss << std::hex << std::setfill ('0');
      for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
         oss << std::setw (2) << (int) aDigest[i];

      sResult = oss.str ();
   }

   return sResult;
}
