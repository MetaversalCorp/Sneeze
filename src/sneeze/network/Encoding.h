// Copyright 2026 Metaversal Corporation. All rights reserved.

#ifndef SNEEZE_NETWORK_ENCODING_H
#define SNEEZE_NETWORK_ENCODING_H

#include <string>

namespace SNEEZE
{
   // Promote a string that's either ASCII, valid UTF-8, or ISO-8859-1 into
   // valid UTF-8. HTTP/1.1 historically allowed ISO-8859-1 in header field
   // values, and many origins still emit Latin-1 bytes (Server, Date locales,
   // etc.). Storing the raw bytes in a std::string and later serialising
   // through nlohmann::json blows up because dump() defaults to strict UTF-8.
   //
   // Pure ASCII and already-valid UTF-8 strings pass through unchanged.
   // Anything else is decoded as Latin-1 and re-encoded as UTF-8, so each
   // byte 0x80-0xFF becomes its two-byte UTF-8 mapping (0xC2 0x80 - 0xC3 0xBF).
   std::string ToUtf8 (const std::string& sIn);
}

#endif // SNEEZE_NETWORK_ENCODING_H
