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

// Build the wasm executable (run from this crate's directory):
//    cargo build --target wasm32-unknown-unknown --release --target-dir target
// Output: target/wasm32-unknown-unknown/release/testapi.wasm
// Always pass --target-dir target; otherwise a CARGO_TARGET_DIR set in the shell
// can redirect the output elsewhere.

#![allow(non_snake_case, non_camel_case_types, dead_code)]

// ---------------------------------------------------------------------------
// API test module
//
// This module owns no scene data. On Open it exercises the read-only object
// API of the sneeze SDK - the typed views hanging off the FABRIC handle - and
// logs every method's result to the developer console. It reads only through
// the objects (Location ().Protocol (), Container ().Trust (), ...); it never
// touches the raw snapshot JSON, which the SDK keeps private. The surfaces it
// walks:
//
//    FABRIC     - Index
//    LOCATION   - Href / Protocol / Host / Pathname / Origin       (5)
//    RESOURCE   - Id / Name                                        (2)
//    SIGNATURE  - Algorithm / IsValid / IsChainTrusted / IsChainExpired (4)
//    AGENT      - Browser_Name / _Version / Engine_Name / _Version / Platform / Language (6)
//    CONTAINER  - Name / Organization / OrganizationHash / Persona /
//                 PersonaHash / Fingerprint / Trust / DisplayName /
//                 DisplayOrganization                              (9)
//    SERVICE/MODULE lists via Fabric::Services / Modules
//    DATA       - Has / Get, plus a typed Get_As read of "Schema"  (2 + typed)
//    CONSOLE    - all 14 methods
//
// NODE's accessors are read-only too, but a NODE is only obtained through a
// scene-mutating call, so it is out of scope for a read-only tester.
// ---------------------------------------------------------------------------

use sneeze::*;
use nanoserde::DeJson;

struct TESTAPI;

// A typed record read out of the fabric's "Data" block at "Schema", to prove
// DATA::Get_As deserializes a data sub-tree into a guest struct.
#[derive(DeJson, Default)]
struct KEY_OBJECT
{
   #[nserde(default)] a                                     : i64,
   #[nserde(default)] b                                     : i64,
   #[nserde(default)] c                                     : i64,
}

#[derive(DeJson, Default)]
struct SCHEMA
{
   #[nserde(default)] nKey                                  : i64,
   #[nserde(default)] sKey                                  : String,
   #[nserde(default)] Key_Object                            : KEY_OBJECT,
   #[nserde(default)] Key_Vector                            : Vec<i64>,
}

impl INSTANCE for TESTAPI
{
   fn Open (pFabric: FABRIC)
   {
      let pConsole = pFabric.Console ();

      pConsole.Group ("testapi: object/method API sweep");

      Test_Fabric    (&pFabric);
      Test_Location  (&pFabric);
      Test_Resource  (&pFabric);
      Test_Signature (&pFabric);
      Test_Agent     (&pFabric);
      Test_Container (&pFabric);
      Test_Manifest  (&pFabric);
      Test_Data      (&pFabric);
      Test_Console   (&pFabric);

      pConsole.Group_End ();
   }

   fn Close (pFabric: FABRIC)
   {
      pFabric.Console ().Log ("testapi: Close");
   }
}

// ---------------------------------------------------------------------------
// FABRIC.
// ---------------------------------------------------------------------------

fn Test_Fabric (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();

   pConsole.Group ("===== FABRIC =====");
   pConsole.Log (&format! ("Index () = {}", pFabric.Index ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// LOCATION - the fabric URL, split web-style.
// ---------------------------------------------------------------------------

fn Test_Location (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pLoc     = pFabric.Location ();

   pConsole.Group ("----- Location () - 5 methods -----");
   pConsole.Log (&format! ("Href     () = {}", pLoc.Href ()));
   pConsole.Log (&format! ("Protocol () = {}", pLoc.Protocol ()));
   pConsole.Log (&format! ("Host     () = {}", pLoc.Host ()));
   pConsole.Log (&format! ("Pathname () = {}", pLoc.Pathname ()));
   pConsole.Log (&format! ("Origin   () = {}", pLoc.Origin ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// RESOURCE - the launching resource's identity.
// ---------------------------------------------------------------------------

fn Test_Resource (pFabric: &FABRIC)
{
   let pConsole  = pFabric.Console ();
   let pResource = pFabric.Resource ();

   pConsole.Group ("----- Resource () - 2 methods -----");
   pConsole.Log (&format! ("Id   () = {}", pResource.Id ()));
   pConsole.Log (&format! ("Name () = {}", pResource.Name ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// SIGNATURE - the MSF verification result.
// ---------------------------------------------------------------------------

fn Test_Signature (pFabric: &FABRIC)
{
   let pConsole   = pFabric.Console ();
   let pSignature = pFabric.Signature ();

   pConsole.Group ("----- Signature () - 4 methods -----");
   pConsole.Log (&format! ("Algorithm      () = {}", pSignature.Algorithm ()));
   pConsole.Log (&format! ("IsValid        () = {}", pSignature.IsValid ()));
   pConsole.Log (&format! ("IsChainTrusted () = {}", pSignature.IsChainTrusted ()));
   pConsole.Log (&format! ("IsChainExpired () = {}", pSignature.IsChainExpired ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// AGENT - host/engine identity (navigator analog).
// ---------------------------------------------------------------------------

fn Test_Agent (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pAgent   = pFabric.Agent ();

   pConsole.Group ("----- Agent () - 6 methods -----");
   pConsole.Log (&format! ("Browser_Name    () = {}", pAgent.Browser_Name ()));
   pConsole.Log (&format! ("Browser_Version () = {}", pAgent.Browser_Version ()));
   pConsole.Log (&format! ("Engine_Name     () = {}", pAgent.Engine_Name ()));
   pConsole.Log (&format! ("Engine_Version  () = {}", pAgent.Engine_Version ()));
   pConsole.Log (&format! ("Platform        () = {}", pAgent.Platform ()));
   pConsole.Log (&format! ("Language        () = {}", pAgent.Language ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// CONTAINER - the container identity, raw fields plus composed display names.
// ---------------------------------------------------------------------------

fn Test_Container (pFabric: &FABRIC)
{
   let pConsole   = pFabric.Console ();
   let pContainer = pFabric.Container ();

   pConsole.Group ("----- Container () - 9 methods -----");
   pConsole.Log (&format! ("Name                () = {}", pContainer.Name ()));
   pConsole.Log (&format! ("Organization        () = {}", pContainer.Organization ()));
   pConsole.Log (&format! ("OrganizationHash    () = {}", pContainer.OrganizationHash ()));
   pConsole.Log (&format! ("Persona             () = {}", pContainer.Persona ()));
   pConsole.Log (&format! ("PersonaHash         () = {}", pContainer.PersonaHash ()));
   pConsole.Log (&format! ("Fingerprint         () = {}", pContainer.Fingerprint ()));
   pConsole.Log (&format! ("Trust               () = {} ({})", pContainer.Trust (), Trust_Name (pContainer.Trust ())));
   pConsole.Log (&format! ("DisplayName         () = {}", pContainer.DisplayName ()));
   pConsole.Log (&format! ("DisplayOrganization () = {}", pContainer.DisplayOrganization ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// SERVICE + MODULE manifest lists.
// ---------------------------------------------------------------------------

fn Test_Manifest (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();

   pConsole.Group (&format! ("----- Services () - {} entries -----", pFabric.Services ().len ()));
   for pService in pFabric.Services ()
   {
      pConsole.Log (&format! ("Name={}  Type={}  Endpoint={}  Modules={:?}", pService.Name (), pService.Type (), pService.Endpoint (), pService.Modules ()));
   }
   pConsole.Group_End ();

   pConsole.Group (&format! ("----- Modules () - {} entries -----", pFabric.Modules ().len ()));
   for pModule in pFabric.Modules ()
   {
      pConsole.Log (&format! ("Url={}  Hash={}", pModule.Url (), pModule.Hash ()));
   }
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// DATA - the read-only "Data" tree: Has/Get, plus a typed Get_As of "Schema".
// ---------------------------------------------------------------------------

fn Test_Data (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pData    = pFabric.Data ();

   pConsole.Group ("----- Data () - Has / Get -----");
   Show_Data (&pConsole, &pData, "");
   Show_Data (&pConsole, &pData, "Scene");
   Show_Data (&pConsole, &pData, "does.not.exist");
   pConsole.Group_End ();

   pConsole.Group ("Data ().Get (\"Schema\") - guest-parsed typed read");
   let pSchema = pData.Get ("Schema").and_then (|sJson| SCHEMA::deserialize_json (&sJson).ok ());
   match pSchema
   {
      Some (pSchema) =>
      {
         pConsole.Log (&format! ("nKey       = {}", pSchema.nKey));
         pConsole.Log (&format! ("sKey       = {}", pSchema.sKey));
         pConsole.Log (&format! ("Key_Object = {{ a:{}, b:{}, c:{} }}", pSchema.Key_Object.a, pSchema.Key_Object.b, pSchema.Key_Object.c));
         pConsole.Log (&format! ("Key_Vector = {:?}", pSchema.Key_Vector));
      }
      None =>
      {
         pConsole.Warn ("Schema absent or failed to parse (add a \"Schema\" object to the fabric's Data to see this populated)");
      }
   }
   pConsole.Group_End ();
}

fn Show_Data (pConsole: &CONSOLE, pData: &DATA, sPath: &str)
{
   let sLabel = if sPath.is_empty () { "(root)" } else { sPath };

   pConsole.Log (&format! ("Has (\"{}\") = {}", sLabel, pData.Has (sPath)));
   pConsole.Log (&format! ("Get (\"{}\") = {}", sLabel, Show_Option (&pData.Get (sPath))));
}

// ---------------------------------------------------------------------------
// CONSOLE - all 14 methods. Log and Group are used throughout the sweep; this
// section exercises the remaining severities and facilities. Assert is called
// with a true condition so it does not fire.
// ---------------------------------------------------------------------------

fn Test_Console (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();

   pConsole.Group ("----- Console () - severities + facilities -----");

   pConsole.Debug ("Debug line");
   pConsole.Info  ("Info line");
   pConsole.Warn  ("Warn line");
   pConsole.Error ("Error line");

   pConsole.Assert (true, "this assert should NOT print (condition is true)");

   pConsole.Count ("tick");
   pConsole.Count ("tick");
   pConsole.Count_Reset ("tick");

   pConsole.Time ("span");
   pConsole.Time_Log ("span");
   pConsole.Time_End ("span");

   pConsole.Group_Collapsed ("collapsed subgroup");
   pConsole.Log ("(inside collapsed group)");
   pConsole.Group_End ();

   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

fn Trust_Name (nTrust: i32) -> &'static str
{
   let sName = match nTrust
   {
      sneeze::abi::kSNEEZE_ABI_TRUST_NONE       => "NONE",
      sneeze::abi::kSNEEZE_ABI_TRUST_UNTRUSTED  => "UNTRUSTED",
      sneeze::abi::kSNEEZE_ABI_TRUST_UNVERIFIED => "UNVERIFIED",
      sneeze::abi::kSNEEZE_ABI_TRUST_EXPIRED    => "EXPIRED",
      sneeze::abi::kSNEEZE_ABI_TRUST_VERIFIED   => "VERIFIED",
      sneeze::abi::kSNEEZE_ABI_TRUST_ROOT       => "ROOT",
      _                                         => "?",
   };

   sName
}

fn Show_Option (pValue: &Option<String>) -> String
{
   let sOut = match pValue
   {
      Some (sText) => Clip (sText),
      None         => String::from ("(none)"),
   };

   sOut
}

// Caps a long string at 200 chars (on a char boundary) so a big data blob does
// not flood the console; appends the true byte length when clipped.
fn Clip (sText: &str) -> String
{
   let sOut = if sText.chars ().count () > 200
   {
      let sHead: String = sText.chars ().take (200).collect ();
      format! ("{}... ({} bytes)", sHead, sText.len ())
   }
   else
   {
      sText.to_string ()
   };

   sOut
}

sneeze::instance! (TESTAPI);
