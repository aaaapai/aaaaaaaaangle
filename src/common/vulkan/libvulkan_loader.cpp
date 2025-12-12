//
// Copyright 2021 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// libvulkan_loader.cpp:
//    Helper functions for the loading Vulkan libraries.
//

#include "common/vulkan/libvulkan_loader.h"

#include "common/system_utils.h"

#include <cstdlib>
#include <dlfcn.h>


static void* vulkan_load_from_pojavexec() {
    // 首先检查环境变量 VULKAN_PTR
    const char* vulkan_ptr_env = std::getenv("VULKAN_PTR");
    if (vulkan_ptr_env) {
        printf("[ANGLE] Use VULKAN_PTR = %s\n", vulkan_ptr_env);
        return (void*)std::strtoul(vulkan_ptr_env, NULL, 0x10);
    }

    const char* turnipEnv = std::getenv("ANGLE_LOAD_TURNIP");
    if (!turnipEnv || (std::string(turnipEnv) != "true")) {
        return nullptr;
    }

    printf("[ANGLE] Try to dlopen libpojavexec.\n");
    void* lib_handle = dlopen("libpojavexec.so", RTLD_NOLOAD);
    if (lib_handle == nullptr) {
      printf("[ANGLE] Failed to dlopen libpojavexec, now try again.\n");
      lib_handle = dlopen("libpojavexec.so", RTLD_LOCAL|RTLD_LAZY);
      if (lib_handle == nullptr) {
          printf("[ANGLE] Failed to dlopen libpojavexec.Are you using Pojav Glow Worm? Now try to dlopen libpgw.\n");
          lib_handle = dlopen("libpgw.so", RTLD_NOLOAD);
          if (lib_handle == nullptr) {
            printf("[ANGLE] Failed to dlopen libpgw.Now try again.\n");
            lib_handle = dlopen("libpgw.so", RTLD_LOCAL|RTLD_LAZY);
          }
      }
    }
    
    // 获取 load_vulkan 函数
    void *(*load_vulkan_func)() = reinterpret_cast<void*(*)()>(dlsym(lib_handle, "maybe_load_vulkan"));
    if (load_vulkan_func) {
        // 调用 load_vulkan 函数
        vulkan_ptr_env = std::getenv("VULKAN_PTR");
        if (vulkan_ptr_env) {
          printf("[ANGLE] Use VULKAN_PTR = %s\n", vulkan_ptr_env);
        }
        return load_vulkan_func();
    }

    return nullptr;
}

namespace angle
{
namespace vk
{
void *OpenLibVulkan()
{

    void* vulkan_load_from_pojavexec_result = vulkan_load_from_pojavexec();
    if (vulkan_load_from_pojavexec_result != nullptr) {
        return vulkan_load_from_pojavexec_result;
    }

    printf("[ANGLE] Warning: No environment variable VULKAN_PTR! Will load libvulkan.\n");
    constexpr const char *kLibVulkanNames[] = {
#if defined(ANGLE_PLATFORM_WINDOWS)
        "vulkan-1.dll",
#elif defined(ANGLE_PLATFORM_APPLE)
        "libvulkan.dylib",
        "libvulkan.1.dylib",
        "libMoltenVK.dylib"
#else
        "libvulkan.so",
        "libvulkan.so.1",
#endif
    };

    const char *kLibVulkanName_env = std::getenv("ANGLE_LIBVULKAN_NAME");

    constexpr SearchType kSearchTypes[] = {
// On Android and Fuchsia we use the system libvulkan.
#if defined(ANGLE_USE_CUSTOM_LIBVULKAN)
        SearchType::ModuleDir,
#else
        SearchType::SystemDir,
#endif  // defined(ANGLE_USE_CUSTOM_LIBVULKAN)
    };

    for (angle::SearchType searchType : kSearchTypes)
    {
        for (const char *libraryName : kLibVulkanNames)
        {
            void *library = nullptr;
            if (kLibVulkanName_env) {
                library = OpenSystemLibraryWithExtension(kLibVulkanName_env, searchType);
            } else {
                library = OpenSystemLibraryWithExtension(libraryName, searchType);
            }
            if (library)
            {
                return library;
            }
        }
    }

    printf("[ANGLE] Error: failed to load libvulkan.\n");
    return nullptr;
}
}  // namespace vk
}  // namespace angle
