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

#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// URL resolution - resolve a reference against the fabric's own URL using the
// relative-reference rules of RFC 3986 section 5.2:
//
//   - A reference with a scheme is absolute and used unchanged.
//   - "//host/path" keeps the base scheme and takes a new authority.
//   - "/path" is taken from the base authority's root.
//   - An empty path ("", "?q", "#f") stays on the base document.
//   - Anything else is relative to the folder holding the base.
//
// Literal dot segments are collapsed, empty segments and trailing slashes are
// preserved, and ".." never climbs above the root. Percent-encoded dots are
// left intact; resolution is not an access-control check.
//
// The parent's query is never inherited by a reference that has a path: a
// reference names the parameters it wants as "<name>" tokens in its own query
// (see Query_Fill). Only a reference that stays on the base document (empty
// path and no query of its own) keeps the base query, since it is the same
// document. A base without "scheme://" is not hierarchical, so the reference
// is returned as written (placeholders still filled).
// ---------------------------------------------------------------------------

namespace SNEEZE
{
namespace UrlResolve
{

struct PARTS
{
   std::string sScheme;
   std::string sAuthority;
   std::string sPath;
   std::string sQuery;
   std::string sFragment;
   bool        bScheme    = false;
   bool        bAuthority = false;
   bool        bQuery     = false;
   bool        bFragment  = false;
};

inline bool IsSchemeChar (char c, bool bFirst)
{
   bool bAlpha = (c >= 'a'  &&  c <= 'z')  ||  (c >= 'A'  &&  c <= 'Z');
   bool bOther = (c >= '0'  &&  c <= '9')  ||  c == '+'  ||  c == '-'  ||  c == '.';

   return bAlpha  ||  (!bFirst  &&  bOther);
}

inline PARTS Parse (const std::string& sUrl)
{
   PARTS  Parts;
   size_t nHash = sUrl.find ('#');
   size_t nEnd  = (nHash == std::string::npos) ? sUrl.length () : nHash;
   size_t nPos  = 0;

   if (nHash != std::string::npos)
   {
      Parts.bFragment = true;
      Parts.sFragment = sUrl.substr (nHash + 1);
   }

   // A scheme needs two or more characters so a Windows drive letter ("C:/x")
   // is not mistaken for one, and it must end before any '/', '?' or '#'.
   if (nEnd > 0  &&  IsSchemeChar (sUrl[0], true))
   {
      size_t nScan = 1;

      while (nScan < nEnd  &&  IsSchemeChar (sUrl[nScan], false))
         nScan++;

      if (nScan < nEnd  &&  nScan >= 2  &&  sUrl[nScan] == ':')
      {
         Parts.bScheme = true;
         Parts.sScheme = sUrl.substr (0, nScan);
         nPos          = nScan + 1;
      }
   }

   size_t nQ = sUrl.find ('?', nPos);

   if (nQ != std::string::npos  &&  nQ < nEnd)
   {
      Parts.bQuery = true;
      Parts.sQuery = sUrl.substr (nQ + 1, nEnd - nQ - 1);
      nEnd         = nQ;
   }

   if (nEnd - nPos >= 2  &&  sUrl[nPos] == '/'  &&  sUrl[nPos + 1] == '/')
   {
      size_t nSlash = sUrl.find ('/', nPos + 2);

      if (nSlash == std::string::npos  ||  nSlash > nEnd)
         nSlash = nEnd;

      Parts.bAuthority = true;
      Parts.sAuthority = sUrl.substr (nPos + 2, nSlash - nPos - 2);
      nPos             = nSlash;
   }

   Parts.sPath = sUrl.substr (nPos, nEnd - nPos);

   return Parts;
}

// RFC 3986 section 5.2.4 on an absolute path. Empty segments are kept, and a
// path ending in "/", "/." or "/.." ends in "/".
inline std::string RemoveDotSegments (const std::string& sPath)
{
   if (sPath.empty ())
      return sPath;

   std::vector<std::string> asSeg;
   size_t                   nStart = (!sPath.empty ()  &&  sPath[0] == '/') ? 1 : 0;
   bool                     bMore  = true;

   while (bMore)
   {
      size_t      nSlash = sPath.find ('/', nStart);
      bool        bLast  = (nSlash == std::string::npos);
      std::string sSeg   = bLast ? sPath.substr (nStart) : sPath.substr (nStart, nSlash - nStart);

      if (sSeg == "..")
      {
         if (!asSeg.empty ())
            asSeg.pop_back ();

         if (bLast)
            asSeg.push_back ("");
      }
      else if (sSeg == ".")
      {
         if (bLast)
            asSeg.push_back ("");
      }
      else
      {
         asSeg.push_back (sSeg);
      }

      bMore  = !bLast;
      nStart = bLast ? nStart : nSlash + 1;
   }

   std::string sResult;

   for (size_t i = 0; i < asSeg.size (); ++i)
   {
      sResult += "/";
      sResult += asSeg[i];
   }

   return sResult.empty () ? std::string ("/") : sResult;
}

// Find the first parameter named sName in an '&'-separated query and return
// its whole token ("name=value", or a bare "name").
inline bool Query_Find (const std::string& sQuery, const std::string& sName, std::string& sParam)
{
   bool   bFound = false;
   size_t nStart = 0;

   while (!bFound  &&  nStart <= sQuery.length ())
   {
      size_t      nAmp   = sQuery.find ('&', nStart);
      size_t      nEnd   = (nAmp == std::string::npos) ? sQuery.length () : nAmp;
      std::string sToken = sQuery.substr (nStart, nEnd - nStart);

      if (sToken.substr (0, sToken.find ('=')) == sName)
      {
         sParam = sToken;
         bFound = true;
      }

      nStart = nEnd + 1;
   }

   return bFound;
}

// A child reference pulls parameters from its parent fabric by name: each
// "<name>" token is replaced by the parent's "name=value", or dropped when the
// parent has no such parameter. Every other token is kept as written.
inline std::string Query_Fill (const std::string& sQuery, const std::string& sQuery_Parent)
{
   std::string sResult;
   size_t      nStart = 0;

   while (nStart <= sQuery.length ())
   {
      size_t      nAmp   = sQuery.find ('&', nStart);
      size_t      nEnd   = (nAmp == std::string::npos) ? sQuery.length () : nAmp;
      std::string sToken = sQuery.substr (nStart, nEnd - nStart);
      std::string sParam;

      if (sToken.length () > 2  &&  sToken.front () == '<'  &&  sToken.back () == '>')
         Query_Find (sQuery_Parent, sToken.substr (1, sToken.length () - 2), sParam);
      else
         sParam = sToken;

      if (!sParam.empty ())
      {
         if (!sResult.empty ())
            sResult += "&";

         sResult += sParam;
      }

      nStart = nEnd + 1;
   }

   return sResult;
}

inline std::string Compose (const PARTS& Parts)
{
   std::string sResult;

   if (Parts.bScheme)
      sResult += Parts.sScheme + ":";

   if (Parts.bAuthority)
      sResult += "//" + Parts.sAuthority;

   sResult += Parts.sPath;

   if (Parts.bQuery)
      sResult += "?" + Parts.sQuery;

   if (Parts.bFragment)
      sResult += "#" + Parts.sFragment;

   return sResult;
}

inline std::string Resolve (const std::string& sBase, const std::string& sReference)
{
   PARTS Base   = Parse (sBase);
   PARTS Ref    = Parse (sReference);
   PARTS Target = Ref;

   bool  bRelative = !Ref.bScheme  &&  Base.bScheme  &&  Base.bAuthority;

   if (bRelative)
   {
      Target.bScheme = true;
      Target.sScheme = Base.sScheme;

      if (Ref.bAuthority)
      {
         Target.sPath = RemoveDotSegments (Ref.sPath);
      }
      else
      {
         Target.bAuthority = true;
         Target.sAuthority = Base.sAuthority;

         if (Ref.sPath.empty ())
         {
            Target.sPath = Base.sPath;

            if (!Ref.bQuery)
            {
               Target.bQuery = Base.bQuery;
               Target.sQuery = Base.sQuery;
            }
         }
         else if (Ref.sPath[0] == '/')
         {
            Target.sPath = RemoveDotSegments (Ref.sPath);
         }
         else
         {
            size_t      nLastSlash = Base.sPath.find_last_of ('/');
            std::string sBaseDir   = (nLastSlash == std::string::npos) ? "/" : Base.sPath.substr (0, nLastSlash + 1);

            Target.sPath = RemoveDotSegments (sBaseDir + Ref.sPath);
         }
      }
   }

   if (Ref.bQuery  &&  Ref.sQuery.find ('<') != std::string::npos)
   {
      Target.sQuery = Query_Fill (Ref.sQuery, Base.sQuery);
      if (Target.sQuery.empty ())
         Target.bQuery = false;
   }

   // Nothing to resolve or fill: the reference goes back out as written.
   if (!bRelative  &&  Target.sQuery == Ref.sQuery)
      return sReference;

   return Compose (Target);
}

}
}
