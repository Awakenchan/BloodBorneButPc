// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

bool HandleEvent(const SDL_Event& event);
void UpdateTextInput(SDL_Window* window);

bool Visible();

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

bool CapturesInput();

bool MenuOpen();

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

}
