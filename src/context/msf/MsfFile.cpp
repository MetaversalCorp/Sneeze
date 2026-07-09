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

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

using namespace SNEEZE;

using SERVICE = MSF::SERVICE;
using MODULE  = MSF::MODULE;

// JSON keys for the MSF payload (the signed fabric-source document). The
// service and module sub-objects have their own key groups; note "modules"
// exists in two contexts (a service's module-name list vs the payload-level
// module array) and so is a distinct constant in each.
#define MSF_KEY_CONTAINER    "container"
#define MSF_KEY_SUCCESSOR    "successor"
#define MSF_KEY_SERVICES     "services"
#define MSF_KEY_MODULES      "modules"
#define SERVICE_KEY_NAME     "name"
#define SERVICE_KEY_TYPE     "type"
#define SERVICE_KEY_ENDPOINT "endpoint"
#define SERVICE_KEY_MODULES  "modules"
#define MODULE_KEY_URL       "url"
#define MODULE_KEY_HASH      "hash"

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

MSF::MSF (ENGINE* pEngine)
   : m_bSignatureValid (false)
   , m_bChainTrusted (false)
   , m_bChainExpired (false)
   , m_bParsed (false)
   , m_pEngine (pEngine)
{
}

MSF::~MSF ()
{
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

bool MSF::Parse (const std::string& sJws, const std::string& sUrl)
{
   bool bResult = false;

   m_pJson_Payload         = nlohmann::json ();
   m_sAlgorithm.clear ();
   m_sFingerprint.clear ();
   m_sOrganization.clear ();
   m_sOrganizationHash.clear ();
   m_sRawJws.clear ();
   m_sSignatureError.clear ();
   m_sChainError.clear ();
   m_bSignatureValid = false;
   m_bChainTrusted   = false;
   m_bChainExpired   = false;
   m_bParsed         = false;
   m_aX5cEntries.clear ();
   m_aCertInfos.clear ();

   if (!sJws.empty ())
   {
      // A plain JSON document starts with '{' or '[' (after an optional BOM
      // and whitespace); a JWS is dot-separated base64url with no leading
      // brace. Detect JSON first so float and URL dots inside a plain JSON
      // payload are not mistaken for JWS segment separators.
      size_t nStart = 0;
      if (sJws.size () >= 3  &&  static_cast<unsigned char> (sJws[0]) == 0xEF  &&  static_cast<unsigned char> (sJws[1]) == 0xBB  &&  static_cast<unsigned char> (sJws[2]) == 0xBF)
         nStart = 3;

      size_t nFirst  = sJws.find_first_not_of (" \t\r\n", nStart);
      bool   bIsJson = (nFirst != std::string::npos)  &&  (sJws[nFirst] == '{'  ||  sJws[nFirst] == '[');
      bool   bIsJws  = !bIsJson  &&  (sJws.find ('.') != std::string::npos);

      if (bIsJws)
      {
         try
         {
            m_sRawJws = sJws;
            auto decoded = jwt::decode (sJws);

            m_sAlgorithm = decoded.get_algorithm ();

            if (decoded.has_header_claim ("x5c"))
            {
               auto aX5cJson = decoded.get_header_claim ("x5c").as_array ();
               for (const auto& entry : aX5cJson)
                  m_aX5cEntries.push_back (entry.get<std::string> ());

               for (size_t i = 0; i < m_aX5cEntries.size (); ++i)
               {
                  m_aCertInfos.push_back (
                     CHAIN::DecodeInfoDerBase64 (m_aX5cEntries[i], i > 0));

                  if (i == 0)
                  {
                     m_sFingerprint      = CHAIN::ComputeFingerprint (m_aX5cEntries[0]);
                     m_sOrganization     = m_aCertInfos[0].sOrganization;
                     m_sOrganizationHash = CHAIN::HashString (m_aCertInfos[0].sSubject);
                  }
               }
            }

            if (decoded.has_payload_claim ("data"))
            {
               std::string sPayloadStr = decoded.get_payload_claim ("data").as_string ();
               try
               {
                  m_pJson_Payload = nlohmann::json::parse (sPayloadStr);
               }
               catch (...)
               {
                  m_pJson_Payload = sPayloadStr;
               }
            }

            m_bParsed = true;
            bResult   = true;
         }
         catch (const std::exception& ex)
         {
            if (m_pEngine)
               m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "MSF", std::string ("Parse: ") + ex.what ());
         }
      }
      else
      {
         try
         {
            m_pJson_Payload = nlohmann::json::parse (sJws);

            m_sFingerprint      = CHAIN::HashString (sUrl + sJws);
            m_sOrganizationHash = CHAIN::HashString (sUrl);

            m_bParsed = true;
            bResult   = true;
         }
         catch (const std::exception& ex)
         {
            if (m_pEngine)
               m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "MSF", std::string ("Parse (JSON): ") + ex.what ());
         }
      }
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Sign
// ---------------------------------------------------------------------------

std::string MSF::Sign (const std::string& sPrivateKeyPem, const std::string& sAlgorithm)
{
   std::string sResult;

   try
   {
      nlohmann::json aX5c = nlohmann::json::array ();
      bool bCertsOk = true;

      for (size_t i = 0; i < m_aCertsPem.size ()  &&  bCertsOk; ++i)
      {
         std::string sB64 = CHAIN::PemToDerBase64 (m_aCertsPem[i]);
         if (sB64.empty ())
            bCertsOk = false;
         else
            aX5c.push_back (sB64);
      }

      if (bCertsOk)
      {
         std::string sPayloadStr = m_pJson_Payload.dump ();

         auto pBuilder = jwt::create ()
            .set_type ("JWS")
            .set_header_claim ("x5c", jwt::claim (aX5c))
            .set_payload_claim ("data", jwt::claim (sPayloadStr));

         if (sAlgorithm == "RS256")
            sResult = pBuilder.sign (jwt::algorithm::rs256 ("", sPrivateKeyPem));
         else if (sAlgorithm == "RS384")
            sResult = pBuilder.sign (jwt::algorithm::rs384 ("", sPrivateKeyPem));
         else if (sAlgorithm == "RS512")
            sResult = pBuilder.sign (jwt::algorithm::rs512 ("", sPrivateKeyPem));
         else if (sAlgorithm == "ES256")
            sResult = pBuilder.sign (jwt::algorithm::es256 ("", sPrivateKeyPem));
         else if (sAlgorithm == "ES384")
            sResult = pBuilder.sign (jwt::algorithm::es384 ("", sPrivateKeyPem));
         else if (sAlgorithm == "ES512")
            sResult = pBuilder.sign (jwt::algorithm::es512 ("", sPrivateKeyPem));
         else
         {
            if (m_pEngine)
               m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "MSF", "Sign: unknown algorithm \"" + sAlgorithm + "\"");
         }
      }
   }
   catch (const std::exception& ex)
   {
      if (m_pEngine)
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "MSF", std::string ("Sign: exception: ") + ex.what ());

      sResult.clear ();
   }

   return sResult;
}

// ---------------------------------------------------------------------------
// VerifySignature
// ---------------------------------------------------------------------------

bool MSF::VerifySignature ()
{
   bool bResult = false;
   m_bSignatureValid = false;
   m_sSignatureError.clear ();

   if (!m_bParsed)
   {
      m_sSignatureError = "no data parsed (call Parse first)";
   }
   else if (m_aX5cEntries.empty ())
   {
      m_sSignatureError = "no certificates in JWS header";
   }
   else
   {
      try
      {
         auto decoded = jwt::decode (m_sRawJws);

         std::string sPubKeyPem = CHAIN::ExtractPublicKeyPem (m_aX5cEntries[0]);
         if (sPubKeyPem.empty ())
            throw std::runtime_error ("failed to extract public key from leaf certificate");

         if (m_sAlgorithm == "RS256")
            jwt::verify ().allow_algorithm (jwt::algorithm::rs256 (sPubKeyPem)).verify (decoded);
         else if (m_sAlgorithm == "RS384")
            jwt::verify ().allow_algorithm (jwt::algorithm::rs384 (sPubKeyPem)).verify (decoded);
         else if (m_sAlgorithm == "RS512")
            jwt::verify ().allow_algorithm (jwt::algorithm::rs512 (sPubKeyPem)).verify (decoded);
         else if (m_sAlgorithm == "ES256")
            jwt::verify ().allow_algorithm (jwt::algorithm::es256 (sPubKeyPem)).verify (decoded);
         else if (m_sAlgorithm == "ES384")
            jwt::verify ().allow_algorithm (jwt::algorithm::es384 (sPubKeyPem)).verify (decoded);
         else if (m_sAlgorithm == "ES512")
            jwt::verify ().allow_algorithm (jwt::algorithm::es512 (sPubKeyPem)).verify (decoded);
         else
            throw std::runtime_error ("unsupported algorithm: " + m_sAlgorithm);

         m_bSignatureValid = true;
         bResult = true;
      }
      catch (const std::exception& ex)
      {
         if (m_sSignatureError.empty ())
            m_sSignatureError = ex.what ();
      }
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// VerifyChain
// ---------------------------------------------------------------------------

bool MSF::VerifyChain ()
{
   bool bResult = false;
   m_bChainTrusted = false;
   m_bChainExpired = false;
   m_sChainError.clear ();

   if (!m_bParsed)
   {
      m_sChainError = "no data parsed (call Parse first)";
   }
   else if (m_aX5cEntries.empty ())
   {
      m_sChainError = "no certificates in JWS header";
   }
   else
   {
      std::string sError;
      if (m_certChain.Validate (m_aX5cEntries, sError))
      {
         m_bChainTrusted = true;
         bResult = true;
      }
      else
      {
         m_sChainError   = sError;
         m_bChainExpired = (sError.find ("expired") != std::string::npos);
      }
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Trust store
// ---------------------------------------------------------------------------

void MSF::AddTrustedCert (const std::string& sPem)
{
   m_certChain.AddTrustedCert (sPem);
}

// ---------------------------------------------------------------------------
// Certificate chain management
// ---------------------------------------------------------------------------

void MSF::AddCert (const std::string& sPem)
{
   bool bIsCA = !m_aCertsPem.empty ();
   m_aCertsPem.push_back (sPem);
   m_aCertInfos.push_back (CHAIN::DecodeInfoPem (sPem, bIsCA));
}

bool MSF::RemoveCert (int nIndex)
{
   bool bResult = false;

   if (nIndex >= 0  &&  nIndex < (int) m_aCertsPem.size ())
   {
      m_aCertsPem.erase (m_aCertsPem.begin () + nIndex);
      if (nIndex < (int) m_aCertInfos.size ())
         m_aCertInfos.erase (m_aCertInfos.begin () + nIndex);
      bResult = true;
   }

   return bResult;
}

const std::vector<MSF::CERT>& MSF::CertInfos () const
{
   return m_aCertInfos;
}

int MSF::CertCount () const
{
   return (int) m_aCertInfos.size ();
}

// ---------------------------------------------------------------------------
// Payload (bulk)
// ---------------------------------------------------------------------------

void MSF::SetPayload (const nlohmann::json& pJson_Payload)
{
   m_pJson_Payload = pJson_Payload;
}

nlohmann::json MSF::Payload () const
{
   return m_pJson_Payload;
}

// ---------------------------------------------------------------------------
// Payload (typed fields)
// ---------------------------------------------------------------------------

void MSF::SetContainer (const std::string& sContainer)
{
   if (!m_pJson_Payload.is_object ())
      m_pJson_Payload = nlohmann::json::object ();
   m_pJson_Payload[MSF_KEY_CONTAINER] = sContainer;
}

std::string MSF::Container () const
{
   std::string sResult;
   if (m_pJson_Payload.is_object ()  &&  m_pJson_Payload.contains (MSF_KEY_CONTAINER))
      sResult = m_pJson_Payload[MSF_KEY_CONTAINER].get<std::string> ();
   return sResult;
}

void MSF::SetSuccessor (const std::string& sSuccessor)
{
   if (!m_pJson_Payload.is_object ())
      m_pJson_Payload = nlohmann::json::object ();
      m_pJson_Payload[MSF_KEY_SUCCESSOR] = sSuccessor;
}

std::string MSF::Successor () const
{
   std::string sResult;
   if (m_pJson_Payload.is_object ()  &&  m_pJson_Payload.contains (MSF_KEY_SUCCESSOR))
      sResult = m_pJson_Payload[MSF_KEY_SUCCESSOR].get<std::string> ();
   return sResult;
}

// ---------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------

void MSF::AddService (const SERVICE& service)
{
   if (!m_pJson_Payload.is_object ())
      m_pJson_Payload = nlohmann::json::object ();
   if (!m_pJson_Payload.contains (MSF_KEY_SERVICES))
      m_pJson_Payload[MSF_KEY_SERVICES] = nlohmann::json::array ();

   nlohmann::json svc;
   svc[SERVICE_KEY_NAME]     = service.sName;
   svc[SERVICE_KEY_TYPE]     = service.sType;
   svc[SERVICE_KEY_ENDPOINT] = service.sEndpoint;
   if (!service.aModules.empty ())
      svc[SERVICE_KEY_MODULES] = service.aModules;

      m_pJson_Payload[MSF_KEY_SERVICES].push_back (svc);
}

bool MSF::RemoveService (const std::string& sName)
{
   bool bResult = false;

   if (m_pJson_Payload.is_object ()  &&  m_pJson_Payload.contains (MSF_KEY_SERVICES)  &&  m_pJson_Payload[MSF_KEY_SERVICES].is_array ())
   {
      auto& aServices = m_pJson_Payload[MSF_KEY_SERVICES];
      for (auto it = aServices.begin (); it != aServices.end (); ++it)
      {
         if (it->contains (SERVICE_KEY_NAME)  &&  (*it)[SERVICE_KEY_NAME].get<std::string> () == sName)
         {
            aServices.erase (it);
            bResult = true;
            break;
         }
      }
   }

   return bResult;
}

std::vector<SERVICE> MSF::Services () const
{
   std::vector<SERVICE> aResult;

   if (m_pJson_Payload.is_object ()  &&  m_pJson_Payload.contains (MSF_KEY_SERVICES)  &&  m_pJson_Payload[MSF_KEY_SERVICES].is_array ())
   {
      for (const auto& svc : m_pJson_Payload[MSF_KEY_SERVICES])
      {
         SERVICE entry;
         if (svc.contains (SERVICE_KEY_NAME))     entry.sName     = svc[SERVICE_KEY_NAME].get<std::string> ();
         if (svc.contains (SERVICE_KEY_TYPE))     entry.sType     = svc[SERVICE_KEY_TYPE].get<std::string> ();
         if (svc.contains (SERVICE_KEY_ENDPOINT)) entry.sEndpoint = svc[SERVICE_KEY_ENDPOINT].get<std::string> ();
         if (svc.contains (SERVICE_KEY_MODULES))
         {
            for (const auto& mod : svc[SERVICE_KEY_MODULES])
               entry.aModules.push_back (mod.get<std::string> ());
         }
         aResult.push_back (entry);
      }
   }

   return aResult;
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

void MSF::AddModule (const std::string& sUrl, const std::string& sHash)
{
   if (!m_pJson_Payload.is_object ())
      m_pJson_Payload = nlohmann::json::object ();
   if (!m_pJson_Payload.contains (MSF_KEY_MODULES))
      m_pJson_Payload[MSF_KEY_MODULES] = nlohmann::json::array ();

   nlohmann::json mod;
   mod[MODULE_KEY_URL]  = sUrl;
   mod[MODULE_KEY_HASH] = sHash;
   m_pJson_Payload[MSF_KEY_MODULES].push_back (mod);
}

bool MSF::RemoveModule (const std::string& sUrl)
{
   bool bResult = false;

   if (m_pJson_Payload.is_object ()  &&  m_pJson_Payload.contains (MSF_KEY_MODULES)  &&  m_pJson_Payload[MSF_KEY_MODULES].is_array ())
   {
      auto& aModules = m_pJson_Payload[MSF_KEY_MODULES];
      for (auto it = aModules.begin (); it != aModules.end (); ++it)
      {
         if (it->contains (MODULE_KEY_URL)  &&  (*it)[MODULE_KEY_URL].get<std::string> () == sUrl)
         {
            aModules.erase (it);
            bResult = true;
            break;
         }
      }
   }

   return bResult;
}

std::vector<MODULE> MSF::Modules () const
{
   std::vector<MODULE> aResult;

   if (m_pJson_Payload.is_object ()  &&  m_pJson_Payload.contains (MSF_KEY_MODULES)  &&  m_pJson_Payload[MSF_KEY_MODULES].is_array ())
   {
      for (auto& jModule : m_pJson_Payload[MSF_KEY_MODULES])
      {
         MODULE mod;
         if (jModule.contains (MODULE_KEY_URL))  mod.sUrl  = jModule[MODULE_KEY_URL].get<std::string> ();
         if (jModule.contains (MODULE_KEY_HASH)) mod.sHash = jModule[MODULE_KEY_HASH].get<std::string> ();
         aResult.push_back (mod);
      }
   }

   return aResult;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

bool        MSF::IsSignatureValid  () const { return m_bSignatureValid; }
bool        MSF::IsChainTrusted    () const { return m_bChainTrusted; }
bool        MSF::IsChainExpired    () const { return m_bChainExpired; }

std::string MSF::Algorithm         () const { return m_sAlgorithm; }
std::string MSF::Fingerprint       () const { return m_sFingerprint; }
std::string MSF::Organization      () const { return m_sOrganization; }
std::string MSF::OrganizationHash  () const { return m_sOrganizationHash; }
std::string MSF::SignatureError    () const { return m_sSignatureError; }
std::string MSF::ChainError        () const { return m_sChainError; }

std::string MSF::DisplayOrganization () const
{
   std::string sResult;

   if (m_bChainTrusted  ||  m_bChainExpired)
      sResult = m_sOrganization;
   else if (!m_sOrganizationHash.empty ())
      sResult = m_sOrganizationHash;

   return sResult;
}
