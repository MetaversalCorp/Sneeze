// Copyright 2026 Metaversal Corporation. All rights reserved.

#include "Encoding.h"

namespace SNEEZE
{

static bool IsAscii (const std::string& sIn)
{
   for (unsigned char c : sIn)
      if (c >= 0x80)
         return false;
   return true;
}

static bool IsValidUtf8 (const std::string& sIn)
{
   size_t i = 0;
   while (i < sIn.size ())
   {
      unsigned char c = static_cast<unsigned char> (sIn[i]);
      size_t n;
      if      (c < 0x80)            { ++i; continue; }
      else if ((c & 0xE0) == 0xC0)  n = 2;
      else if ((c & 0xF0) == 0xE0)  n = 3;
      else if ((c & 0xF8) == 0xF0)  n = 4;
      else                          return false;

      if (i + n > sIn.size ())
         return false;
      for (size_t k = 1; k < n; ++k)
         if ((static_cast<unsigned char> (sIn[i + k]) & 0xC0) != 0x80)
            return false;
      i += n;
   }
   return true;
}

std::string ToUtf8 (const std::string& sIn)
{
   if (IsAscii (sIn) || IsValidUtf8 (sIn))
      return sIn;

   std::string sOut;
   sOut.reserve (sIn.size () * 2);
   for (unsigned char c : sIn)
   {
      if (c < 0x80) sOut.push_back (static_cast<char> (c));
      else
      {
         sOut.push_back (static_cast<char> (0xC0 | (c >> 6)));
         sOut.push_back (static_cast<char> (0x80 | (c & 0x3F)));
      }
   }
   return sOut;
}

} // namespace SNEEZE
