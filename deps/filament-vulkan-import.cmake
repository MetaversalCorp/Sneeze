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

# Overlay for Filament v1.71.0.mv.2: implement VulkanDriver::importTextureR so
# Texture::Builder::import() can wrap an OpenXR swapchain VkImage. Upstream
# Vulkan importTextureR is a stub that asserts. This lives in Sneeze so the
# Filament GitHub repo does not need a new commit or tag.
#
# cmake -DSOURCE_DIR=<filament clone> -P filament-vulkan-import.cmake
# Idempotent: skips when the overlay is already present.

if (NOT SOURCE_DIR)
   message (FATAL_ERROR "filament-vulkan-import.cmake: SOURCE_DIR is not set")
endif ()

set (_file "${SOURCE_DIR}/filament/backend/src/vulkan/VulkanDriver.cpp")
if (NOT EXISTS "${_file}")
   message (STATUS "filament-vulkan-import: ${_file} not present; skip")
   return ()
endif ()

file (READ "${_file}" _src)
set (_crlf FALSE)
if (_src MATCHES "\r\n")
   set (_crlf TRUE)
   string (REPLACE "\r\n" "\n" _src "${_src}")
endif ()

set (_needle
"void VulkanDriver::importTextureR(Handle<HwTexture> th, intptr_t id,
        SamplerType target, uint8_t levels,
        TextureFormat format, uint8_t samples, uint32_t w, uint32_t h, uint32_t depth,
        TextureUsage usage, utils::ImmutableCString&& tag) {
    // not supported in this backend
    assert_invariant(false && \"Not supported in Vulkan backend\");
    mResourceManager.associateHandle(th.getId(), std::move(tag));
}
")

set (_replacement
"void VulkanDriver::importTextureR(Handle<HwTexture> th, intptr_t id,
        SamplerType target, uint8_t levels,
        TextureFormat format, uint8_t samples, uint32_t w, uint32_t h, uint32_t depth,
        TextureUsage usage, utils::ImmutableCString&& tag) {
    FVK_SYSTRACE_SCOPE();
    (void)target;
    (void)levels;
    VkImage image = reinterpret_cast<VkImage>(id);
    VkFormat vkFormat = fvkutils::getVkFormat(format);
    auto texture = resource_ptr<VulkanTexture>::make(&mResourceManager, th, mContext,
            mPlatform->getDevice(), mAllocator, &mResourceManager, &mCommands, image,
            VK_NULL_HANDLE, vkFormat, VK_NULL_HANDLE, samples, w, h, depth, usage, mStagePool);
    texture.inc();
    mResourceManager.associateHandle(th.getId(), std::move(tag));
}
")

string (FIND "${_src}" "${_needle}" _pos)
if (_pos GREATER_EQUAL 0)
   string (REPLACE "${_needle}" "${_replacement}" _src "${_src}")
   if (_crlf)
      string (REPLACE "\n" "\r\n" _src "${_src}")
   endif ()
   file (WRITE "${_file}" "${_src}")
   message (STATUS "filament-vulkan-import: applied Vulkan importTextureR overlay")
elseif (_src MATCHES "VkImage image = reinterpret_cast<VkImage>\\(id\\)")
   message (STATUS "filament-vulkan-import: overlay already present")
else ()
   message (FATAL_ERROR
      "filament-vulkan-import: could not find the Vulkan importTextureR stub in:\n"
      "  ${_file}\n"
      "Expected Filament v1.71.0.mv.2 (stub asserts 'Not supported in Vulkan backend').")
endif ()
