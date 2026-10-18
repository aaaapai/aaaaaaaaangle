//
// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WindowSurfaceVkAndroid.cpp:
//    Implements the class methods for WindowSurfaceVkAndroid.
//

#include "libANGLE/renderer/vulkan/android/WindowSurfaceVkAndroid.h"

#include <android/native_window.h>

#include "libANGLE/renderer/vulkan/vk_renderer.h"

namespace rx
{

WindowSurfaceVkAndroid::WindowSurfaceVkAndroid(const egl::SurfaceState &surfaceState,
                                               EGLNativeWindowType window)
    : WindowSurfaceVk(surfaceState, window)
{}

angle::Result WindowSurfaceVkAndroid::createSurfaceVk(vk::ErrorContext *context)
{
    VkAndroidSurfaceCreateInfoKHR createInfo = {};

    createInfo.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.flags  = 0;
    createInfo.window = mNativeWindowType;
    ANGLE_VK_TRY(context, VK_CALL(vkCreateAndroidSurfaceKHR, context->getRenderer()->getInstance(),
                                  &createInfo, nullptr, &mSurface));

    return angle::Result::Continue;
}

angle::Result WindowSurfaceVkAndroid::getCurrentWindowSize(vk::ErrorContext *context,
                                                           gl::Extents *extentsOut) const
{
    // ANativeWindow_getWidth/Height reflects the buffer geometry set by
    // ANativeWindow_setBuffersGeometry, which may not yet be reflected in
    // surfaceCaps.currentExtent on some drivers.
    int width  = ANativeWindow_getWidth(mNativeWindowType);
    int height = ANativeWindow_getHeight(mNativeWindowType);

    if (ANGLE_UNLIKELY(width <= 0 || height <= 0))
    {
        // Fallback to surface capabilities if ANativeWindow query fails.
        vk::Renderer *renderer                 = context->getRenderer();
        const VkPhysicalDevice &physicalDevice = renderer->getPhysicalDevice();
        VkSurfaceCapabilitiesKHR surfaceCaps;
        ANGLE_VK_TRY(context, VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
                                      physicalDevice, mSurface, &surfaceCaps));
        width  = surfaceCaps.currentExtent.width;
        height = surfaceCaps.currentExtent.height;
    }

    *extentsOut = gl::Extents(width, height, 1);
    return angle::Result::Continue;
}

}  // namespace rx
