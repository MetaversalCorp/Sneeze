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

//! The typed snapshot views. The engine pushes one immutable JSON document at
//! Open; the SDK parses it once (privately) into SNAPSHOT_DATA and exposes each
//! section as a read-only view class reached through the FABRIC handle
//! (RESOURCE / CONTAINER / SIGNATURE / AGENT / SERVICE / MODULE), plus LOCATION,
//! a URL parser derived from RESOURCE's reference. A module never touches the
//! raw JSON: fields are private, read through getter methods. Every field is
//! defaulted, so a missing, partial, or rearranged blob yields empty values
//! rather than a failure.

use nanoserde::DeJson;
use crate::abi::kSNEEZE_ABI_TRUST_EXPIRED;

// ---------------------------------------------------------------------------
// RESOURCE - the launching resource (the attaching node's map object). Id is a
// decimal string on the wire (a u64 on the host, beyond JSON's safe range).
// LOCATION derives from sReference (not exposed directly - it is Location.Href).
// ---------------------------------------------------------------------------

#[derive(Clone, Default, DeJson)]
pub struct RESOURCE
{
   #[nserde(default)] qwResource                            : String,
   #[nserde(default)] sName                                 : String,
   #[nserde(default)] sReference                            : String,
}

impl RESOURCE
{
   pub fn Id   (&self) -> u64  { self.qwResource.parse ().unwrap_or (0) }
   pub fn Name (&self) -> &str { &self.sName }

   pub (crate) fn Reference (&self) -> &str { &self.sReference }
}

// ---------------------------------------------------------------------------
// CONTAINER - the container identity (CID). Raw fields plus the two composed
// display names (mirroring CID::DisplayName / MSF::DisplayOrganization). Trust
// is the eSNEEZE_ABI_TRUST integer (compare against the kSNEEZE_ABI_TRUST_*
// constants in the abi module).
// ---------------------------------------------------------------------------

#[derive(Clone, Default, DeJson)]
pub struct CONTAINER
{
   #[nserde(default)] sContainer                            : String,
   #[nserde(default)] sOrganization                         : String,
   #[nserde(default)] sOrganizationHash                     : String,
   #[nserde(default)] sPersona                              : String,
   #[nserde(default)] sPersonaHash                          : String,
   #[nserde(default)] sFingerprint                          : String,
   #[nserde(default)] eTrust                                : i32,
}

impl CONTAINER
{
   pub fn Name             (&self) -> &str { &self.sContainer }
   pub fn Organization     (&self) -> &str { &self.sOrganization }
   pub fn OrganizationHash (&self) -> &str { &self.sOrganizationHash }
   pub fn Persona          (&self) -> &str { &self.sPersona }
   pub fn PersonaHash      (&self) -> &str { &self.sPersonaHash }
   pub fn Fingerprint      (&self) -> &str { &self.sFingerprint }
   pub fn Trust            (&self) -> i32  { self.eTrust }

   /// Friendly container name, composed guest-side like CID::DisplayName: the
   /// friendly organization (or its hash, below the expired-trust threshold)
   /// joined to the container name.
   pub fn DisplayName (&self) -> String
   {
      let sOrg = if self.eTrust >= kSNEEZE_ABI_TRUST_EXPIRED { &self.sOrganization } else { &self.sOrganizationHash };

      format! ("{}/{}", sOrg, self.sContainer)
   }

   /// Friendly organization name, composed guest-side like MSF::DisplayOrganization:
   /// the friendly name once the chain is trusted-or-expired, else the hash.
   pub fn DisplayOrganization (&self) -> String
   {
      let sResult = if self.eTrust >= kSNEEZE_ABI_TRUST_EXPIRED
      {
         self.sOrganization.clone ()
      }
      else
      {
         self.sOrganizationHash.clone ()
      };

      sResult
   }
}

// ---------------------------------------------------------------------------
// SIGNATURE - the MSF verification result.
// ---------------------------------------------------------------------------

#[derive(Clone, Default, DeJson)]
pub struct SIGNATURE
{
   #[nserde(default)] sAlgorithm                            : String,
   #[nserde(default)] bSignatureValid                       : bool,
   #[nserde(default)] bChainTrusted                         : bool,
   #[nserde(default)] bChainExpired                         : bool,
}

impl SIGNATURE
{
   pub fn Algorithm      (&self) -> &str { &self.sAlgorithm }
   pub fn IsValid        (&self) -> bool { self.bSignatureValid }
   pub fn IsChainTrusted (&self) -> bool { self.bChainTrusted }
   pub fn IsChainExpired (&self) -> bool { self.bChainExpired }
}

// ---------------------------------------------------------------------------
// AGENT - host-supplied identity (navigator analog).
// ---------------------------------------------------------------------------

#[derive(Clone, Default, DeJson)]
pub struct AGENT
{
   #[nserde(default)] sBrowser_Name                         : String,
   #[nserde(default)] sBrowser_Version                      : String,
   #[nserde(default)] sEngine_Name                          : String,
   #[nserde(default)] sEngine_Version                       : String,
   #[nserde(default)] sPlatform                             : String,
   #[nserde(default)] sLanguage                             : String,
}

impl AGENT
{
   pub fn Browser_Name    (&self) -> &str { &self.sBrowser_Name }
   pub fn Browser_Version (&self) -> &str { &self.sBrowser_Version }
   pub fn Engine_Name     (&self) -> &str { &self.sEngine_Name }
   pub fn Engine_Version  (&self) -> &str { &self.sEngine_Version }
   pub fn Platform        (&self) -> &str { &self.sPlatform }
   pub fn Language        (&self) -> &str { &self.sLanguage }
}

// ---------------------------------------------------------------------------
// SERVICE - a declared service: name/type/endpoint plus the module names it uses.
// ---------------------------------------------------------------------------

#[derive(Clone, Default, DeJson)]
pub struct SERVICE
{
   #[nserde(default)] sName                                 : String,
   #[nserde(default)] sType                                 : String,
   #[nserde(default)] sEndpoint                             : String,
   #[nserde(default)] aModules                              : Vec<String>,
}

impl SERVICE
{
   pub fn Name     (&self) -> &str        { &self.sName }
   pub fn Type     (&self) -> &str        { &self.sType }
   pub fn Endpoint (&self) -> &str        { &self.sEndpoint }
   pub fn Modules  (&self) -> &[String]   { &self.aModules }
}

// ---------------------------------------------------------------------------
// MODULE - a declared wasm module (url + SRI hash).
// ---------------------------------------------------------------------------

#[derive(Clone, Default, DeJson)]
pub struct MODULE
{
   #[nserde(default)] sUrl                                  : String,
   #[nserde(default)] sHash                                 : String,
}

impl MODULE
{
   pub fn Url  (&self) -> &str { &self.sUrl }
   pub fn Hash (&self) -> &str { &self.sHash }
}

// ---------------------------------------------------------------------------
// LOCATION - a URL, split into its parts. Derived from RESOURCE.sReference at
// Open (Fabric::Location), but also constructable directly (LOCATION::New) so it
// doubles as a general URL parser, like the web's URL class. Read-only getters
// mirror the web location: Href / Protocol / Host / Pathname / Origin.
// ---------------------------------------------------------------------------

#[derive(Clone, Default)]
pub struct LOCATION
{
   sHref                                                    : String,
   sProtocol                                                : String,
   sHost                                                    : String,
   sPathname                                                : String,
}

impl LOCATION
{
   pub fn New (sUrl: &str) -> Self
   {
      let mut sProtocol = String::new ();
      let mut sHost     = String::new ();
      let mut sPathname = String::from ("/");

      let sRest = match sUrl.find ("://")
      {
         Some (nScheme) =>
         {
            sProtocol = format! ("{}:", &sUrl[..nScheme]);
            &sUrl[nScheme + 3..]
         }
         None =>
         {
            ""
         }
      };

      if sProtocol.is_empty ()
      {
         sPathname = sUrl.to_string ();
      }
      else
      {
         match sRest.find ('/')
         {
            Some (nPath) =>
            {
               sHost     = sRest[..nPath].to_string ();
               sPathname = sRest[nPath..].to_string ();
            }
            None =>
            {
               sHost = sRest.to_string ();
            }
         }
      }

      LOCATION { sHref: sUrl.to_string (), sProtocol, sHost, sPathname }
   }

   pub fn Href     (&self) -> &str { &self.sHref }
   pub fn Protocol (&self) -> &str { &self.sProtocol }
   pub fn Host     (&self) -> &str { &self.sHost }
   pub fn Pathname (&self) -> &str { &self.sPathname }

   /// Scheme + host, e.g. "https://cdn.rp1.com". Empty when there is no scheme.
   pub fn Origin (&self) -> String
   {
      let sResult = if self.sProtocol.is_empty ()
      {
         String::new ()
      }
      else
      {
         format! ("{}//{}", self.sProtocol, self.sHost)
      };

      sResult
   }
}

// ---------------------------------------------------------------------------
// SNAPSHOT_DATA - the whole Open snapshot parsed into owned records. Crate-
// internal: FABRIC hands out references into its sections; a module never sees
// this aggregate or the raw JSON. Section field names match the pushed JSON keys.
// ---------------------------------------------------------------------------

#[derive(Clone, Default, DeJson)]
pub struct SNAPSHOT_DATA
{
   #[nserde(default)] pub (crate) Resource                  : RESOURCE,
   #[nserde(default)] pub (crate) Container                 : CONTAINER,
   #[nserde(default)] pub (crate) Signature                 : SIGNATURE,
   #[nserde(default)] pub (crate) Agent                     : AGENT,
   #[nserde(default)] pub (crate) Services                  : Vec<SERVICE>,
   #[nserde(default)] pub (crate) Modules                   : Vec<MODULE>,
}
