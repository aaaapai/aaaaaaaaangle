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
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <charconv>
#include <format>

static void print_message(std::string_view msg) {
    fwrite(msg.data(), 1, msg.size(), stdout);
}

#if !defined(ANGLE_PLATFORM_APPLE)
static void* vulkan_load_from_env() {
    const char* vulkan_ptr_env = std::getenv("VULKAN_PTR");
    const char* turnipEnv = std::getenv("ANGLE_LOAD_VULKAN_FROM_ENV"); // depends on launchers....

    if (vulkan_ptr_env && turnipEnv && std::string(turnipEnv) == "true") {
        std::string msg = std::format("[ANGLE] Use VULKAN_PTR = {}\n", vulkan_ptr_env);
        print_message(msg);
        unsigned long value = 0;
        auto [ptr, ec] = std::from_chars(vulkan_ptr_env,
                                         vulkan_ptr_env + std::strlen(vulkan_ptr_env),
                                         value, 16);
        if (ec != std::errc() || value == 0) {
            print_message("[ANGLE] VULKAN_PTR parse failed\n");
            return nullptr;
        }
        return reinterpret_cast<void*>(value);
    }
    
    return nullptr;

}

static void* vulkan_load_from_pojavexec() {
    const char* turnipEnv = std::getenv("ANGLE_LOAD_VULKAN_FROM_POJAVEXEC");

    if (!turnipEnv || std::string(turnipEnv) != "true") {
        return nullptr;
    }

    print_message("[ANGLE] Try to dlopen libpojavexec.\n");
    void* lib_handle = dlopen("libpojavexec.so", RTLD_NOLOAD);
    if (lib_handle == nullptr) {
        print_message("[ANGLE] Failed to dlopen libpojavexec, now try again.\n");
        lib_handle = dlopen("libpojavexec.so", RTLD_LOCAL | RTLD_LAZY);
        if (lib_handle == nullptr) {
            print_message("[ANGLE] Failed to dlopen libpojavexec. Are you using Pojav Glow Worm? Now try to dlopen libpgw.\n");
            lib_handle = dlopen("libpgw.so", RTLD_NOLOAD);
            if (lib_handle == nullptr) {
                print_message("[ANGLE] Failed to dlopen libpgw. Now try again.\n");
                lib_handle = dlopen("libpgw.so", RTLD_LOCAL | RTLD_LAZY);
            }
        }
    }

    void *(*load_vulkan_func)() = reinterpret_cast<void*(*)()>(dlsym(lib_handle, "maybe_load_vulkan"));
    if (load_vulkan_func) {
       return load_vulkan_func();
    }

    return nullptr;
}

static void* vulkan_load_from_special_path() {
    // 首先检查环境变量 VULKAN_PTR
    const char* vulkan_ptr_env = std::getenv("VULKAN_PTR");
    const char* turnipEnv = std::getenv("ANGLE_LOAD_TURNIP");
    if (vulkan_ptr_env && turnipEnv) {
        printf("[ANGLE] Use VULKAN_PTR = %s\n", vulkan_ptr_env);
        return (void*)std::strtoul(vulkan_ptr_env, NULL, 0x10);
    }

    if (!turnipEnv || (std::string(turnipEnv) != "false")) {
        return nullptr;
    }

    printf("[ANGLE] Try to dlopen libpojavexec.\n");
    void* lib_handle = dlopen("libpojavexec.so", RTLD_NOLOAD);
    if (lib_handle == nullptr) {
      printf("[ANGLE] Failed to dlopen libpojavexec, now try again.\n");
      lib_handle = dlopen("libpojavexec.so", RTLD_LOCAL|RTLD_LAZY);
      if (lib_handle == nullptr) {
          print_message("[ANGLE] Failed to dlopen libpojavexec.Are you using Pojav Glow Worm? Now try to dlopen libpgw.\n");
          lib_handle = dlopen("libpgw.so", RTLD_NOLOAD);
          if (lib_handle == nullptr) {
            print_message("[ANGLE] Failed to dlopen libpgw.Now try again.\n");
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
#endif


namespace angle
{
namespace vk
{
void *OpenLibVulkan()
{
#if !defined(ANGLE_PLATFORM_APPLE)
    // Turnip in Pojav Backend seems to be not able to be loaded twice.
    void* vulkan_load_result = vulkan_load_from_special_path();
    if (vulkan_load_result == nullptr) {
        vulkan_load_result = vulkan_load_from_env();
    }
    if (vulkan_load_result == nullptr) {
        vulkan_load_result = vulkan_load_from_pojavexec();
    }
    if (vulkan_load_result != nullptr) {
        return vulkan_load_result;
    }

    print_message("[ANGLE] WARN: No VULKAN_PTR from pojavexec or env! vulkan_loader will load libvulkan.\n");
#endif

    constexpr const char *kLibVulkanNames[] = {
#if defined(ANGLE_PLATFORM_WINDOWS)
        "vulkan-1.dll",
#elif defined(ANGLE_PLATFORM_APPLE)
        "libvulkan.dylib", "libMoltenVK.dylib", "libvulkan.1.dylib",
        // Fallback paths for static macOS builds where the Vulkan loader is bundled
        // in the "Libraries/" subdirectory but the host module (containing ANGLE)
        // is in the parent directory.
        "Libraries/libvulkan.dylib", "Libraries/libvulkan.1.dylib", "Libraries/libMoltenVK.dylib"
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

    print_message("[ANGLE] ERROR: failed to load libvulkan or libMoltenVK.\n");
    return nullptr;
}
}  // namespace vk
}  // namespace angle
