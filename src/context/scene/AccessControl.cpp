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

#include "AccessControl.h"

using namespace SNEEZE;


// ---------------------------------------------------------------------------
// CanRead -- determines if pRequestingOwner may read this node's data.
//
// Rules:
//   1. Null owner = browser internal, always allowed.
//   2. If the node is private and the requester doesn't own the fabric, deny.
//   3. If the node's fabric is private and the requester doesn't own it, deny.
//   4. Otherwise, allow.
// ---------------------------------------------------------------------------

bool CanRead (const NODE* pNode, const void* pRequestingOwner)
{
   if (!pRequestingOwner)
      return true;

   if (!pNode)
      return false;

   const FABRIC* pFabric = pNode->Fabric ();
   if (!pFabric)
      return true;

   if (pNode->IsPrivate ()  &&  pFabric->Container () != pRequestingOwner)
      return false;

   return true;
}

// ---------------------------------------------------------------------------
// CanWrite -- determines if pRequestingOwner may modify this node.
//
// Rules:
//   1. Null owner = browser internal, always allowed.
//   2. The requester must own the fabric that owns this node.
// ---------------------------------------------------------------------------

bool CanWrite (const NODE* pNode, const void* pRequestingOwner)
{
   if (!pRequestingOwner)
      return true;

   if (!pNode)
      return false;

   const FABRIC* pFabric = pNode->Fabric ();
   if (!pFabric)
      return false;

   return pFabric->Container () == pRequestingOwner;
}

// ---------------------------------------------------------------------------
// Fabric-level access
// ---------------------------------------------------------------------------

bool CanReadFabric (const FABRIC* pFabric, const void* pRequestingOwner)
{
   if (!pRequestingOwner)
      return true;

   if (!pFabric)
      return false;

   return true;
}

bool CanWriteFabric (const FABRIC* pFabric, const void* pRequestingOwner)
{
   if (!pRequestingOwner)
      return true;

   if (!pFabric)
      return false;

   return pFabric->Container () == pRequestingOwner;
}
