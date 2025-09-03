// Copyright 2022, Simon Zeni <simon@bl4ckb0ne.ca>
// Copyright 2022-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Displays the content of one or both eye onto a desktop window
 * @author Simon Zeni <simon@bl4ckb0ne.ca>
 * @ingroup comp_main
 */

#include "main/comp_compositor.h"
#include "main/comp_target_swapchain.h"
#include "main/comp_window_peek.h"

#include "util/u_debug.h"
#include "util/u_string_list.h"

#ifdef XRT_HAVE_SDL2
#include <SDL2/SDL.h>
#else
#error "comp_window_peek.h requires SDL2"
#endif
#include <SDL2/SDL_vulkan.h>


DEBUG_GET_ONCE_OPTION(window_peek, "XRT_WINDOW_PEEK", NULL)

#define PEEK_IMAGE_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

struct comp_window_peek
{
	struct comp_target_swapchain base;
	struct comp_compositor *c;

	enum comp_window_peek_eye eye;
	SDL_Window *window;
	uint32_t width, height;
	bool running;
	bool hidden;

	struct vk_cmd_pool pool;
	VkCommandBuffer cmd;

	struct os_thread_helper oth;
};


static inline struct vk_bundle *
get_vk(struct comp_window_peek *w)
{
	return &w->c->base.vk;
}

static inline void
create_images(struct comp_window_peek *w)
{
	struct comp_target_create_images_info info = {
	    .extent = {w->width, w->height},
	    .color_space = w->c->settings.color_space,
	    .image_usage = PEEK_IMAGE_USAGE,
	    .present_mode = VK_PRESENT_MODE_MAILBOX_KHR,
	};

	static_assert(ARRAY_SIZE(info.formats) == ARRAY_SIZE(w->c->settings.formats), "Miss-match format array sizes");
	for (uint32_t i = 0; i < w->c->settings.format_count; i++) {
		info.formats[info.format_count++] = w->c->settings.formats[i];
	}

	comp_target_create_images(&w->base.base, &info);
}

static void *
window_peek_run_thread(void *ptr)
{
	struct comp_window_peek *w = ptr;

	w->running = true;
	w->hidden = false;
	while (w->running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_QUIT: w->running = false; break;
			case SDL_WINDOWEVENT:
				switch (event.window.event) {
				case SDL_WINDOWEVENT_HIDDEN: w->hidden = true; break;
				case SDL_WINDOWEVENT_SHOWN: w->hidden = false; break;
				case SDL_WINDOWEVENT_SIZE_CHANGED:
					w->width = event.window.data1;
					w->height = event.window.data2;
					break;
#if SDL_VERSION_ATLEAST(2, 0, 18)
				case SDL_WINDOWEVENT_DISPLAY_CHANGED:
#endif
				case SDL_WINDOWEVENT_MOVED:
					SDL_GetWindowSize(w->window, (int *)&w->width, (int *)&w->height);
					break;
				default: break;
				}
				break;
			case SDL_KEYDOWN:
				switch (event.key.keysym.sym) {
				case SDLK_ESCAPE: w->running = false; break;
				default: break;
				}
				break;
			default: break;
			}

			if (event.type == SDL_QUIT) {
				w->running = false;
			}
		}
	}

	return NULL;
}

struct comp_window_peek *
comp_window_peek_create(struct comp_compositor *c)
{
	const char *compute = getenv("XRT_COMPOSITOR_COMPUTE");
	if (compute) {
		COMP_WARN(c, "Peek window cannot be enabled on compute compositor");
		return NULL;
	}

	const char *option = debug_get_option_window_peek();
	if (option == NULL) {
		return NULL;
	}

	struct xrt_device *xdev = c->xdev;
	enum comp_window_peek_eye eye = -1;

	int32_t width, height;
	if (strcmp(option, "both") == 0 || strcmp(option, "BOTH") == 0 || strcmp(option, "") == 0) {
		eye = COMP_WINDOW_PEEK_EYE_BOTH;
		width = xdev->hmd->screens[0].w_pixels;
		height = xdev->hmd->screens[0].h_pixels;
	} else if (strcmp(option, "left") == 0 || strcmp(option, "LEFT") == 0) {
		eye = COMP_WINDOW_PEEK_EYE_LEFT;
		width = xdev->hmd->views[0].display.w_pixels;
		height = xdev->hmd->views[0].display.h_pixels;
	} else if (strcmp(option, "right") == 0 || strcmp(option, "RIGHT") == 0) {
		eye = COMP_WINDOW_PEEK_EYE_RIGHT;
		width = xdev->hmd->views[1].display.w_pixels;
		height = xdev->hmd->views[1].display.h_pixels;
	} else {
		COMP_ERROR(c, "XRT_window_peek invalid option '%s'", option);
		COMP_ERROR(c, "must be one of 'both', 'left' or 'right'");
		return NULL;
	}

	COMP_DEBUG(c, "Creating peek window from %s eye(s)", option);

	struct comp_window_peek *w = U_TYPED_CALLOC(struct comp_window_peek);
	w->c = c;
	w->eye = eye;


	/*
	 * Vulkan
	 */

	struct vk_bundle *vk = get_vk(w);

	VkResult ret = vk_cmd_pool_init(vk, &w->pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(c, "vk_cmd_pool_init: %s", vk_result_string(ret));
		goto err_free;
	}

	VK_NAME_COMMAND_POOL(vk, w->pool.pool, "comp_window_peek command pool");

	ret = vk_cmd_pool_create_cmd_buffer(vk, &w->pool, &w->cmd);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(c, "vk_cmd_pool_create_cmd_buffer: %s", vk_result_string(ret));
		goto err_pool;
	}

	VK_NAME_COMMAND_BUFFER(vk, w->cmd, "comp_window_peek command buffer");


	/*
	 * SDL
	 */

	// Only initialize SDL if it hasn't been initialized yet
	if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
		if (SDL_Init(SDL_INIT_VIDEO) < 0) {
			COMP_ERROR(c, "Failed to init SDL2");
			goto err_pool;
		}
	}


	int x = SDL_WINDOWPOS_UNDEFINED;
	int y = SDL_WINDOWPOS_UNDEFINED;

	int flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN;

	w->window = SDL_CreateWindow(xdev->str, x, y, width, height, flags);
	if (w->window == NULL) {
		COMP_ERROR(c, "Failed to create SDL window: %s", SDL_GetError());
		goto err_pool;
	}

	w->width = width;
	w->height = height;

	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);



	w->base.base.name = "peek";
	w->base.base.c = c;
	w->base.display = VK_NULL_HANDLE;

	if (!SDL_Vulkan_CreateSurface(w->window, vk->instance, &w->base.surface.handle)) {
		COMP_ERROR(c, "Failed to create SDL surface: %s", SDL_GetError());
		goto err_window;
	}


	/*
	 * Images
	 */

	create_images(w);

	/*
	 * Thread
	 */

	os_thread_helper_init(&w->oth);
	os_thread_helper_start(&w->oth, window_peek_run_thread, w);

	return w;


err_window:
	SDL_DestroyWindow(w->window);

err_pool:
	vk_cmd_pool_destroy(vk, &w->pool);

err_free:
	free(w);

	return NULL;
}

void
comp_window_peek_destroy(struct comp_window_peek **w_ptr)
{
	struct comp_window_peek *w = *w_ptr;
	if (w == NULL) {
		return;
	}

	// Finish the SDL window loop
	w->running = false;
	os_thread_helper_destroy(&w->oth);


	struct vk_bundle *vk = get_vk(w);

	os_mutex_lock(&vk->queue_mutex);
	vk->vkDeviceWaitIdle(vk->device);
	os_mutex_unlock(&vk->queue_mutex);

	vk_cmd_pool_lock(&w->pool);
	vk->vkFreeCommandBuffers(vk->device, w->pool.pool, 1, &w->cmd);
	vk_cmd_pool_unlock(&w->pool);

	vk_cmd_pool_destroy(vk, &w->pool);

	comp_target_swapchain_cleanup(&w->base);

	SDL_DestroyWindow(w->window);

	free(w);

	*w_ptr = NULL;
}

void
comp_window_peek_blit(struct comp_window_peek *w, VkImage src, int32_t width, int32_t height)
{
	if (w->hidden || !w->running) {
		return;
	}

	if (w->width != w->base.base.width || w->height != w->base.base.height) {
		COMP_DEBUG(w->c, "Resizing swapchain");
		create_images(w);
	}

	while (!comp_target_check_ready(&w->base.base))
		;

	uint32_t current;
	VkResult ret = comp_target_acquire(&w->base.base, &current);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(w->c, "comp_target_acquire: %s", vk_result_string(ret));
	}

	VkImage dst = w->base.base.images[current].handle;

	struct vk_bundle *vk = get_vk(w);

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};

	// For writing and submitting commands.
	vk_cmd_pool_lock(&w->pool);

	ret = vk->vkBeginCommandBuffer(w->cmd, &begin_info);

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};

	// Barrier to make source a source
	vk_cmd_image_barrier_locked(                       //
	    vk,                                            // vk_bundle
	    w->cmd,                                        // cmdbuffer
	    src,                                           // image
	    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,          // srcAccessMask
	    VK_ACCESS_TRANSFER_READ_BIT,                   // dstAccessMask
	    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,      // oldImageLayout
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,          // newImageLayout
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // srcStageMask
	    VK_PIPELINE_STAGE_TRANSFER_BIT,                // dstStageMask
	    range);                                        // subresourceRange

	// Barrier to make destination a destination
	vk_cmd_image_barrier_locked(              //
	    vk,                                   // vk_bundle
	    w->cmd,                               // cmdbuffer
	    dst,                                  // image
	    0,                                    // srcAccessMask
	    VK_ACCESS_TRANSFER_WRITE_BIT,         // dstAccessMask
	    VK_IMAGE_LAYOUT_UNDEFINED,            // oldImageLayout
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // newImageLayout
	    VK_PIPELINE_STAGE_TRANSFER_BIT,       // srcStageMask
	    VK_PIPELINE_STAGE_TRANSFER_BIT,       // dstStageMask
	    range);                               // subresourceRange

	VkImageBlit blit = {
	    .srcSubresource =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .layerCount = 1,
	        },
	    .dstSubresource =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .layerCount = 1,
	        },
	};

	blit.srcOffsets[1].x = width;
	blit.srcOffsets[1].y = height;
	blit.srcOffsets[1].z = 1;

	blit.dstOffsets[1].x = w->width;
	blit.dstOffsets[1].y = w->height;
	blit.dstOffsets[1].z = 1;

	vk->vkCmdBlitImage(                       //
	    w->cmd,                               // commandBuffer
	    src,                                  // srcImage
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // srcImageLayout
	    dst,                                  // dstImage
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // dstImageLayout
	    1,                                    // regionCount
	    &blit,                                // pRegions
	    VK_FILTER_LINEAR                      // filter
	);

	// Reset destination
	vk_cmd_image_barrier_locked(              //
	    vk,                                   // vk_bundle
	    w->cmd,                               // cmdbuffer
	    dst,                                  // image
	    VK_ACCESS_TRANSFER_WRITE_BIT,         // srcAccessMask
	    0,                                    // dstAccessMask
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // oldImageLayout
	    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,      // newImageLayout
	    VK_PIPELINE_STAGE_TRANSFER_BIT,       // srcStageMask
	    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dstStageMask
	    range);                               // subresourceRange

	// Reset src
	vk_cmd_image_barrier_locked(                  //
	    vk,                                       // vk_bundle
	    w->cmd,                                   // cmdbuffer
	    src,                                      // image
	    VK_ACCESS_TRANSFER_READ_BIT,              // srcAccessMask
	    0,                                        // dstAccessMask
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,     // oldImageLayout
	    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, // newImageLayout
	    VK_PIPELINE_STAGE_TRANSFER_BIT,           // srcStageMask
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,        // dstStageMask
	    range);                                   // subresourceRange

	ret = vk->vkEndCommandBuffer(w->cmd);
	if (ret != VK_SUCCESS) {
		vk_cmd_pool_unlock(&w->pool);
		VK_ERROR(vk, "Error: Could not end command buffer.\n");
		return;
	}

	VkPipelineStageFlags submit_flags = VK_PIPELINE_STAGE_TRANSFER_BIT;

	// Waits for command to finish.
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .pNext = NULL,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &w->base.base.semaphores.present_complete,
	    .pWaitDstStageMask = &submit_flags,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &w->cmd,
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores = &w->base.base.semaphores.render_complete,
	};

	// Done writing commands, submit to queue.
	ret = vk_cmd_submit_locked(vk, &vk->main_queue, 1, &submit, VK_NULL_HANDLE);

	// Done submitting commands, unlock pool.
	vk_cmd_pool_unlock(&w->pool);

	// Check results from submit.
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "Error: Could not submit to queue.\n");
		return;
	}

	VkPresentInfoKHR present = {
	    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
	    .pNext = NULL,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &w->base.base.semaphores.render_complete,
	    .swapchainCount = 1,
	    .pSwapchains = &w->base.swapchain.handle,
	    .pImageIndices = &current,
	    .pResults = NULL,
	};

	os_mutex_lock(&vk->queue_mutex);
	ret = vk->vkQueuePresentKHR(vk->main_queue.queue, &present);
	os_mutex_unlock(&vk->queue_mutex);

	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "Error: could not present to queue.\n");
		return;
	}
}

enum comp_window_peek_eye
comp_window_peek_get_eye(struct comp_window_peek *w)
{
	return w->eye;
}

bool
comp_window_peek_get_vk_instance_exts(struct u_string_list *out_required_list)
{
	if (out_required_list == NULL) {
		U_LOG_E("comp_window_peek: out_required_list is null.");
		return false;
	}

	// Only initialize SDL if it hasn't been initialized yet
	if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
		if (SDL_Init(SDL_INIT_VIDEO) < 0) {
			U_LOG_E("comp_window_peek: Failed to init SDL2");
			return false;
		}
	}

	/*!
	 * NOTE: The SDL2 function SDL_Vulkan_GetInstanceExtensions requires an SDL_Window
	 *       but the compositor needs to know which vk (instance) extensions are required
	 *       much earlier than when comp_window_peek is created.
	 *
	 *       API docs for SDL_Vulkan_GetInstanceExtensions states that in future versions
	 *       this parameter will be removed so for now just create a temporary, tiny window.
	 */

	const char **instance_ext_names = NULL;
	const int x = SDL_WINDOWPOS_UNDEFINED;
	const int y = SDL_WINDOWPOS_UNDEFINED;
	const int flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN;
	SDL_Window *tmp_window = SDL_CreateWindow(__func__, x, y, 2, 2, flags);
	if (tmp_window == NULL) {
		U_LOG_E("comp_window_peek: Failed create temp SDL_Window for getting vk instance extensions.");
		return false;
	}

	/*!
	 * WARNING: The extension name strings will not be deeply copied, the docs for
	 *          SDL_Vulkan_GetInstanceExtensions do not state the life time of these strings.
	 *
	 *          based on looking the current codebase these are copied from function-level static
	 *          arrays containing string literals (expanded from vulkan header macros, e.g. VK_FOO_EXTENSION_NAME)
	 * 			so we can assume it's safe to pass around but future versions this may change,
	 *          they may get destroyed when deleting the SDL_Window (although check note comment above).
	 */

	uint32_t size = 0;
	if (!SDL_Vulkan_GetInstanceExtensions(tmp_window, &size, NULL)) {
		U_LOG_E("comp_window_peek: Failed Failed get vk instance extensions for SDL2.");
		goto err_free;
	}

	if (size > 0) {
		instance_ext_names = U_TYPED_ARRAY_CALLOC(const char *, size);
		if (!SDL_Vulkan_GetInstanceExtensions(tmp_window, &size, instance_ext_names)) {
			U_LOG_E("comp_window_peek: Failed Failed get vk instance extensions for SDL2.");
			goto err_free;
		}
	}

	for (uint32_t i = 0; i < size; ++i) {
		if (u_string_list_append_unique(out_required_list, instance_ext_names[i]) == 0) {
			U_LOG_T("comp_window_peek: required instance extension: %s already exits, ignored.",
			        instance_ext_names[i]);
		} else {
			U_LOG_T("comp_window_peek: added required instance extension: %s", instance_ext_names[i]);
		}
	}

err_free:
	free(instance_ext_names);
	SDL_DestroyWindow(tmp_window);
	return true;
}
