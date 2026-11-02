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
# ExternalProject PATCH_COMMAND for RmlUi 6.2. A parent Element can still hold
# an ElementPtr to a child whose destructor has already run (pool slots are not
# scribbled). ~Element then virtual-calls that child and aborts the host when a
# panel closes. Mark the element dead on entry to ~Element and drop the owning
# pointer instead of calling through it. UnloadDocument also guards an empty
# document focus history before calling back().
#
# Idempotent: an already-patched tree is a no-op. Does not move HEAD. A 6.2
# tree that matches neither the stock text nor the patched text is an error, so
# a future pin bump cannot silently skip the fix.
#
# Invoke as:
#   cmake -DSOURCE_DIR=<RmlUi clone> -P rmlui-element-lifetime.cmake

if (NOT DEFINED SOURCE_DIR)
   message (FATAL_ERROR "rmlui-element-lifetime.cmake requires -DSOURCE_DIR=<RmlUi clone>")
endif ()

set (_element_h "${SOURCE_DIR}/Include/RmlUi/Core/Element.h")
set (_element_cpp "${SOURCE_DIR}/Source/Core/Element.cpp")
set (_context_cpp "${SOURCE_DIR}/Source/Core/Context.cpp")

foreach (_file IN ITEMS "${_element_h}" "${_element_cpp}" "${_context_cpp}")
   if (NOT EXISTS "${_file}")
      message (FATAL_ERROR "rmlui-element-lifetime.cmake: missing ${_file}")
   endif ()
endforeach ()

file (READ "${_element_h}" _h)
file (READ "${_element_cpp}" _cpp)
file (READ "${_context_cpp}" _ctx)

set (_mark_h "bool AbandonIfDead(ElementPtr& element);")
set (_mark_cpp "bool Element::AbandonIfDead(ElementPtr& element)")
set (_mark_ctx "OnElementDetach may already have erased every history entry.")

string (FIND "${_h}" "${_mark_h}" _have_h)
string (FIND "${_cpp}" "${_mark_cpp}" _have_cpp)
string (FIND "${_ctx}" "${_mark_ctx}" _have_ctx)

set (_have 0)
if (NOT _have_h EQUAL -1)
   math (EXPR _have "${_have} + 1")
endif ()
if (NOT _have_cpp EQUAL -1)
   math (EXPR _have "${_have} + 1")
endif ()
if (NOT _have_ctx EQUAL -1)
   math (EXPR _have "${_have} + 1")
endif ()

if (_have EQUAL 3)
   return ()
endif ()
if (NOT _have EQUAL 0)
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: RmlUi tree is only partly patched. Restore tag 6.2 or finish the patch by hand.")
endif ()

set (_old_0 [===[	void SetParent(Element* parent);

	void SetDataModel(DataModel* new_data_model);

]===])
set (_new_0 [===[	void SetParent(Element* parent);

	// True when the element was already destroyed. Drops the owning pointer
	// without Release(); the pool slot is already on the freelist.
	bool AbandonIfDead(ElementPtr& element);

	void SetDataModel(DataModel* new_data_model);

]===])
string (FIND "${_h}" "${_old_0}" _at)
string (FIND "${_h}" "${_old_0}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _h does not contain patch hunk 0 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_0}" "${_new_0}" _h "${_h}")

set (_old_1 [===[
	bool visible; // True if the element is visible and active.

	bool offset_fixed;
]===])
set (_new_1 [===[
	bool visible; // True if the element is visible and active.
	// Cleared on entry to ~Element and left clear after the destructor returns.
	// Pool slots are not scribbled, so a parent can still hold a pointer to a
	// child that was already released. Virtual calls on that child go through
	// the base vtable and crash. Parents must drop the pointer instead.
	bool alive;

	bool offset_fixed;
]===])
string (FIND "${_h}" "${_old_1}" _at)
string (FIND "${_h}" "${_old_1}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _h does not contain patch hunk 1 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_1}" "${_new_1}" _h "${_h}")

set (_old_0 [===[Element::Element(const String& tag) :
	local_stacking_context(false), local_stacking_context_forced(false), stacking_context_dirty(false), computed_values_are_default_initialized(true),
	visible(true), offset_fixed(false), absolute_offset_dirty(true), rounded_main_padding_size_dirty(true), dirty_definition(false),
	dirty_child_definitions(false), dirty_animation(false), dirty_transition(false), dirty_transform(false), dirty_perspective(false), tag(tag),
	relative_offset_base(0, 0), relative_offset_position(0, 0), absolute_offset(0, 0), scroll_offset(0, 0)
]===])
set (_new_0 [===[Element::Element(const String& tag) :
	local_stacking_context(false), local_stacking_context_forced(false), stacking_context_dirty(false), computed_values_are_default_initialized(true),
	visible(true), alive(true), offset_fixed(false), absolute_offset_dirty(true), rounded_main_padding_size_dirty(true), dirty_definition(false),
	dirty_child_definitions(false), dirty_animation(false), dirty_transition(false), dirty_transform(false), dirty_perspective(false), tag(tag),
	relative_offset_base(0, 0), relative_offset_position(0, 0), absolute_offset(0, 0), scroll_offset(0, 0)
]===])
string (FIND "${_cpp}" "${_old_0}" _at)
string (FIND "${_cpp}" "${_old_0}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _cpp does not contain patch hunk 0 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_0}" "${_new_0}" _cpp "${_cpp}")

set (_old_1 [===[	RMLUI_ASSERT(parent == nullptr);

	PluginRegistry::NotifyElementDestroy(this);

]===])
set (_new_1 [===[	RMLUI_ASSERT(parent == nullptr);

	// Publish death before any callback. A parent that still owns this element
	// (the crash on panel close) must not virtual-call into it.
	alive = false;

	PluginRegistry::NotifyElementDestroy(this);

]===])
string (FIND "${_cpp}" "${_old_1}" _at)
string (FIND "${_cpp}" "${_old_1}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _cpp does not contain patch hunk 1 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_1}" "${_new_1}" _cpp "${_cpp}")

set (_old_2 [===[	for (ElementPtr& child : children)
	{
		Element* child_ancestor = child.get();
		for (int i = 0; i <= ChildNotifyLevels && child_ancestor; i++, child_ancestor = child_ancestor->GetParentNode())
			child_ancestor->OnChildRemove(child.get());

		child->SetParent(nullptr);
	}

]===])
set (_new_2 [===[	for (ElementPtr& child : children)
	{
		if (!child || AbandonIfDead(child))
			continue;

		Element* child_raw = child.get();
		Element* child_ancestor = child_raw;
		for (int i = 0; i <= ChildNotifyLevels && child_ancestor; i++, child_ancestor = child_ancestor->GetParentNode())
		{
			// This element has already cleared `alive`. It still receives the
			// removal notice; any other ancestor that was already destroyed does not.
			if (child_ancestor != this && !child_ancestor->alive)
				break;
			child_ancestor->OnChildRemove(child_raw);
		}

		if (!AbandonIfDead(child))
			child->SetParent(nullptr);
	}

]===])
string (FIND "${_cpp}" "${_old_2}" _at)
string (FIND "${_cpp}" "${_old_2}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _cpp does not contain patch hunk 2 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_2}" "${_new_2}" _cpp "${_cpp}")

set (_old_3 [===[
	ElementMetaPool::element_meta_pool->pool.DestroyAndDeallocate(meta);
}

]===])
set (_new_3 [===[
	ElementMetaPool::element_meta_pool->pool.DestroyAndDeallocate(meta);
}

bool Element::AbandonIfDead(ElementPtr& element)
{
	bool abandoned = false;

	if (element && !element->alive)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			Log::Message(Log::LT_WARNING, "Element child was already destroyed; dropping the dangling pointer.");
		}
		element.release();
		abandoned = true;
	}

	return abandoned;
}

]===])
string (FIND "${_cpp}" "${_old_3}" _at)
string (FIND "${_cpp}" "${_old_3}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _cpp does not contain patch hunk 3 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_3}" "${_new_3}" _cpp "${_cpp}")

set (_old_4 [===[		owner_document = document;
		for (ElementPtr& child : children)
			child->SetOwnerDocument(document);
	}
}
]===])
set (_new_4 [===[		owner_document = document;
		for (ElementPtr& child : children)
		{
			if (child && !AbandonIfDead(child))
				child->SetOwnerDocument(document);
		}
	}
}
]===])
string (FIND "${_cpp}" "${_old_4}" _at)
string (FIND "${_cpp}" "${_old_4}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _cpp does not contain patch hunk 4 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_4}" "${_new_4}" _cpp "${_cpp}")

set (_old_5 [===[
	for (ElementPtr& child : children)
		child->SetDataModel(new_data_model);
}

]===])
set (_new_5 [===[
	for (ElementPtr& child : children)
	{
		if (child && !AbandonIfDead(child))
			child->SetDataModel(new_data_model);
	}
}

]===])
string (FIND "${_cpp}" "${_old_5}" _at)
string (FIND "${_cpp}" "${_old_5}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _cpp does not contain patch hunk 5 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_5}" "${_new_5}" _cpp "${_cpp}")

set (_old_6 [===[		dirty_child_definitions = false;
		for (const ElementPtr& child : children)
			child->dirty_definition = true;
	}
}
]===])
set (_new_6 [===[		dirty_child_definitions = false;
		for (const ElementPtr& child : children)
		{
			if (child && child->alive)
				child->dirty_definition = true;
		}
	}
}
]===])
string (FIND "${_cpp}" "${_old_6}" _at)
string (FIND "${_cpp}" "${_old_6}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _cpp does not contain patch hunk 6 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_6}" "${_new_6}" _cpp "${_cpp}")

set (_old_0 [===[
	// Focus to the previous document if the old document is the current focus.
	if (focus && focus->GetOwnerDocument() == document)
	{
		focus = nullptr;
		document_focus_history.back()->GetFocusLeafNode()->Focus();
	}

]===])
set (_new_0 [===[
	// Focus to the previous document if the old document is the current focus.
	// OnElementDetach may already have erased every history entry. back() on
	// an empty list is undefined and runs during the same unload that destroys
	// the document tree.
	if (focus && focus->GetOwnerDocument() == document)
	{
		focus = nullptr;
		if (!document_focus_history.empty())
			document_focus_history.back()->GetFocusLeafNode()->Focus();
	}

]===])
string (FIND "${_ctx}" "${_old_0}" _at)
string (FIND "${_ctx}" "${_old_0}" _at_last REVERSE)
if (_at EQUAL -1 OR NOT (_at EQUAL _at_last))
   message (FATAL_ERROR "rmlui-element-lifetime.cmake: _ctx does not contain patch hunk 0 exactly once. RmlUi 6.2 moved; update this script.")
endif ()
string (REPLACE "${_old_0}" "${_new_0}" _ctx "${_ctx}")

file (WRITE "${_element_h}" "${_h}")
file (WRITE "${_element_cpp}" "${_cpp}")
file (WRITE "${_context_cpp}" "${_ctx}")
message (STATUS "Patched RmlUi element lifetime (${SOURCE_DIR})")
