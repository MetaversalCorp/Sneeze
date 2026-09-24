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

#ifndef SNEEZE_PERSONA_PERSONA_H
#define SNEEZE_PERSONA_PERSONA_H

namespace SNEEZE
{
   namespace persona
   {
      // ---------------------------------------------------------------------------
      // PERSONA - temporary local identity proxy.
      //
      // A user "logs in" with a first name and optional second name. The combined
      // "First.Second" string is hashed (SHA-256) to produce a persona key used
      // to scope stores and storage.
      // ---------------------------------------------------------------------------

      class PERSONA
      {
      public:
         explicit PERSONA (SNEEZE::ENGINE* pEngine);

         bool IsLoggedIn () const { return m_bLoggedIn; }

         void Login (const std::string& sFirst, const std::string& sSecond);
         void Logout ();

         const std::string& Name () const { return m_sName; }
         const std::string& Hash () const { return m_sHash; }

      private:
         static std::string ComputeHash (const std::string& sInput);

         SNEEZE::ENGINE* m_pEngine;
         bool        m_bLoggedIn;
         std::string m_sName;
         std::string m_sHash;
      };
   } // namespace persona
}

#endif // SNEEZE_PERSONA_PERSONA_H
