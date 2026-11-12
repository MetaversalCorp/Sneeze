# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ExternalProject PATCH_COMMAND for RmlUi 6.2. Context::UnloadAllDocuments loops
# `while (root->GetNumChildren(true) > 0) UnloadDocument(child 0's owner)`.
# UnloadDocument only detaches a document whose parent is the root, so a root
# child it cannot remove (a dead child whose owner_document is stale, a document
# already queued in unloaded_documents, or a child with no owner document at
# all) leaves the count unchanged and the loop spins forever, re-running
# UpdateHoverChain each pass. ~Context calls this from Rml::RemoveContext, which
# the host reaches while holding its global RmlUi lock and the container Live
# lock, so one stuck context freezes the whole process.
#
# Stop as soon as a pass makes no progress and log it. ~Context's root.reset()
# destroys whatever is left; ~Element already tolerates dead children
# (see rmlui-element-lifetime.cmake).
#
# Idempotent: an already-patched tree is a no-op. Does not move HEAD. A 6.2
# tree that matches neither the stock text nor the patched text is an error, so
# a future pin bump cannot silently skip the fix.
#
# Invoke as:
#   cmake -DSOURCE_DIR=<RmlUi clone> -P rmlui-unload-all.cmake

if (NOT DEFINED SOURCE_DIR)
   message (FATAL_ERROR "rmlui-unload-all.cmake requires -DSOURCE_DIR=<RmlUi clone>")
endif ()

set (_context_cpp "${SOURCE_DIR}/Source/Core/Context.cpp")

if (NOT EXISTS "${_context_cpp}")
   message (FATAL_ERROR "rmlui-unload-all.cmake: missing ${_context_cpp}")
endif ()

file (READ "${_context_cpp}" _ctx)

set (_mark_ctx "UnloadAllDocuments made no progress")
string (FIND "${_ctx}" "${_mark_ctx}" _have_ctx)

if (_have_ctx EQUAL -1)
   set (_old_0 [===[void Context::UnloadAllDocuments()
{
	// Unload all children.
	while (root->GetNumChildren(true) > 0)
		UnloadDocument(root->GetChild(0)->GetOwnerDocument());

]===])
   set (_new_0 [===[void Context::UnloadAllDocuments()
{
	// Unload all children. Stop when a pass removes nothing: a root child that
	// UnloadDocument cannot detach would otherwise spin here forever while the
	// caller holds the host's global locks. ~Context destroys whatever is left.
	while (root->GetNumChildren(true) > 0)
	{
		const int num_children = root->GetNumChildren(true);
		ElementDocument* document = root->GetChild(0)->GetOwnerDocument();

		if (!document)
		{
			Log::Message(Log::LT_WARNING, "UnloadAllDocuments made no progress: root child has no owner document (context '%s').", name.c_str());
			break;
		}

		UnloadDocument(document);

		if (root->GetNumChildren(true) >= num_children)
		{
			Log::Message(Log::LT_WARNING, "UnloadAllDocuments made no progress: could not detach a root child (context '%s').", name.c_str());
			break;
		}
	}

]===])
   string (FIND "${_ctx}" "${_old_0}" _at)
   string (FIND "${_ctx}" "${_old_0}" _at_last REVERSE)
   if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
      message (FATAL_ERROR "rmlui-unload-all.cmake: Context.cpp does not contain patch hunk 0 exactly once. RmlUi 6.2 moved; update this script.")
   endif ()
   string (REPLACE "${_old_0}" "${_new_0}" _ctx "${_ctx}")

   file (WRITE "${_context_cpp}" "${_ctx}")
   message (STATUS "Patched RmlUi UnloadAllDocuments (${SOURCE_DIR})")
endif ()
