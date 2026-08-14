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

#ifndef SNEEZE_MSF_H
#define SNEEZE_MSF_H

namespace SNEEZE
{
   class MSF
   {
   public:
      // --- Nested types ---

      struct MODULE
      {
         std::string sUrl;
         std::string sHash;
      };

      struct CERT
      {
         std::string sSubject;
         std::string sIssuer;
         std::string sOrganization;
         std::string sSerial;
         std::string sNotBefore;
         std::string sNotAfter;
         std::string sKeyType;
         int         nKeyBits;
         bool        bIsCA;
      };

      class CHAIN
      {
      public:
         CHAIN ();
         ~CHAIN ();

         CHAIN (const CHAIN&) = delete;
         CHAIN& operator= (const CHAIN&) = delete;
         CHAIN (CHAIN&&) = delete;
         CHAIN& operator= (CHAIN&&) = delete;

         bool Validate (const std::vector<std::string>& aX5cEntries, std::string& sError);

         std::string LeafFingerprint () const;

         const std::vector<CERT>& Certs () const;

         void TrustedCert_Add (const std::string& sPem);

         static CERT        Cert_DecodeDerBase64 (const std::string& sB64, bool bIsCA);
         static CERT        Cert_DecodePem       (const std::string& sPem, bool bIsCA);
         static std::string Fingerprint_Compute  (const std::string& sB64Der);
         static std::string PublicKey_Extract    (const std::string& sB64Der);
         static std::string Pem_ToDerBase64      (const std::string& sPem);
         static std::string String_Hash          (const std::string& sInput);

      private:
         void TrustStore_Load ();

         struct IMPL;
         IMPL*              m_pImpl;
         std::vector<CERT>  m_aCertInfos;
      };

      // --- Lifecycle ---

      explicit MSF (ENGINE* pEngine = nullptr);
      ~MSF ();

      MSF (const MSF&) = delete;
      MSF& operator= (const MSF&) = delete;
      MSF (MSF&&) = delete;
      MSF& operator= (MSF&&) = delete;

      // --- Parse & Export ---

      bool        Parse (const std::string& sJws, const std::string& sUrl);
      std::string Sign  (const std::string& sPrivateKeyPem,
                         const std::string& sAlgorithm = "RS256");

      // --- Verification ---

      bool Signature_Verify ();
      bool Chain_Verify ();

      // --- Trust store ---

      void TrustedCert_Add (const std::string& sPem);

      // --- Certificate chain ---

      void                      Cert_Add    (const std::string& sPem);
      bool                      Cert_Remove (int nIndex);
      const std::vector<CERT>&  Certs       () const;
      int                       Cert_Count  () const;

      // --- Payload (bulk) ---

      void                  Payload (const nlohmann::json& payload);
      const nlohmann::json& Payload () const;

      // --- Payload (typed fields) ---

      void        Container  (const std::string& sContainer);
      std::string Container  () const;
      void        Successor  (const std::string& sSuccessor);
      std::string Successor  () const;

      // --- Services (a name-keyed object; each service carries arbitrary fields) ---

      void                       Service_Add    (const std::string& sName, const nlohmann::json& service);
      bool                       Service_Remove (const std::string& sName);
      bool                       Service_Has    (const std::string& sName) const;
      nlohmann::json             Service        (const std::string& sName) const;
      std::vector<std::string>   Service_Names  () const;

      // --- Modules ---

      void                  Module_Add    (const std::string& sUrl, const std::string& sHash);
      bool                  Module_Remove (const std::string& sUrl);
      std::vector<MODULE>   Modules       () const;

      // --- Status ---

      bool        IsSignatureValid    () const;
      bool        IsChainTrusted      () const;
      bool        IsChainExpired      () const;

      std::string Algorithm           () const;
      std::string Fingerprint         () const;
      std::string Organization        () const;
      std::string OrganizationHash    () const;
      std::string DisplayOrganization () const;
      std::string SignatureError      () const;
      std::string ChainError          () const;

   private:
      nlohmann::json             m_pJson_Payload;
      std::string                m_sAlgorithm;
      std::string                m_sFingerprint;
      std::string                m_sOrganization;
      std::string                m_sOrganizationHash;
      std::string                m_sRawJws;
      std::string                m_sSignatureError;
      std::string                m_sChainError;
      bool                       m_bSignatureValid;
      bool                       m_bChainTrusted;
      bool                       m_bChainExpired;
      bool                       m_bParsed;

      std::vector<std::string>   m_aX5cEntries;
      std::vector<CERT>          m_aCertInfos;
      std::vector<std::string>   m_aCertsPem;

      CHAIN                      m_certChain;
      ENGINE*                    m_pEngine;
   };
}
#endif // SNEEZE_MSF_H
