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

//! The single host import (`Sneeze.Call`) and the packet writer that feeds it.

// The one guest -> host crossover. The guest packs a request into its own linear
// memory and passes (offset, size); the host reads the header, routes on
// (wType, wMethod), and returns an i64.
#[link(wasm_import_module = "Sneeze")]
extern "C"
{
   fn Call (nOffset: i32, nSize: i32) -> i64;
}

pub(crate) fn CallHost (nOffset: i32, nSize: i32) -> i64
{
   unsafe { Call (nOffset, nSize) }
}

/// Builds one request packet: an 8-byte header (wType, wMethod, dwSize) followed
/// by little-endian scalar fields. Send patches the size and calls the host.
///
/// String and byte arguments are passed by (offset, length) into the guest's own
/// memory, so the referenced buffer must outlive Send (it always does - the
/// caller holds it as a local across the synchronous call).
pub(crate) struct PACKET
{
   m_aByte: Vec<u8>,
}

impl PACKET
{
   pub fn New (wType: u16, wMethod: u16) -> Self
   {
      let mut aByte = Vec::with_capacity (48);

      aByte.extend_from_slice (&wType.to_le_bytes ());
      aByte.extend_from_slice (&wMethod.to_le_bytes ());
      aByte.extend_from_slice (&0u32.to_le_bytes ());   // dwSize placeholder, patched in Send

      PACKET { m_aByte: aByte }
   }

   pub fn Write_Qword (&mut self, qwValue: u64) -> &mut Self
   {
      self.m_aByte.extend_from_slice (&qwValue.to_le_bytes ());
      self
   }

   pub fn Write_Number (&mut self, nValue: i32) -> &mut Self
   {
      self.m_aByte.extend_from_slice (&nValue.to_le_bytes ());
      self
   }

   pub fn Write_Double (&mut self, dValue: f64) -> &mut Self
   {
      self.m_aByte.extend_from_slice (&dValue.to_le_bytes ());
      self
   }

   pub fn Write_Bytes (&mut self, pByte: *const u8, nLength: usize) -> &mut Self
   {
      self.Write_Number (pByte as u32 as i32);
      self.Write_Number (nLength as i32);
      self
   }

   pub fn Write_Text (&mut self, sText: &str) -> &mut Self
   {
      self.Write_Bytes (sText.as_ptr (), sText.len ())
   }

   pub fn Send (mut self) -> i64
   {
      let nSize = (self.m_aByte.len () - 8) as u32;

      self.m_aByte[4..8].copy_from_slice (&nSize.to_le_bytes ());

      CallHost (self.m_aByte.as_ptr () as u32 as i32, self.m_aByte.len () as i32)
   }
}
