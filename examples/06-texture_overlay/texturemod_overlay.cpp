/*
 * Copyright (C) 2021 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include <imgui.h>
#include <reshade.hpp>
#include "descriptor_set_tracking.hpp"
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <unordered_set>

using namespace reshade::api;

struct tex_data
{
	resource_desc desc;
	resource_view last_view;
	uint64_t last_used;
};

struct tex_hash
{
	inline size_t operator()(resource_view value) const
	{
		return static_cast<size_t>(value.handle);
	}
};

struct __declspec(uuid("f326a1eb-5062-453e-9852-a787594a977a")) cmd_data
{
	std::unordered_set<resource_view, tex_hash> current_texture_list;
};

struct __declspec(uuid("0ce51b56-a973-4104-bcca-945686f50170")) device_data
{
	resource green_texture = {};
	resource_view green_texture_srv = {};
	resource_view replaced_texture_srv = {};
	std::unordered_set<resource_view, tex_hash> current_texture_list;
	std::map<resource, tex_data> total_texture_list;
	std::vector<resource_view> destroyed_views;
	uint64_t frame_index = 0;

	bool filter = false;
	float scale = 1.0f;
};

static std::mutex s_mutex;

static void on_init_device(device *device)
{
	auto &data = device->create_private_data<device_data>();

	constexpr uint32_t GREEN = 0xff00ff00;

	subresource_data initial_data;
	initial_data.data = const_cast<uint32_t *>(&GREEN);
	initial_data.row_pitch = sizeof(GREEN);
	initial_data.slice_pitch = sizeof(GREEN);

	if (!device->create_resource(resource_desc(1, 1, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::gpu_only, resource_usage::shader_resource), &initial_data, resource_usage::shader_resource, &data.green_texture))
	{
		reshade::log_message(1, "Failed to create green texture!");
	}
	if (!device->create_resource_view(data.green_texture, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm), &data.green_texture_srv))
	{
		reshade::log_message(1, "Failed to create green texture view!");
	}
}
static void on_destroy_device(device *device)
{
	auto &data = device->get_private_data<device_data>();

	device->destroy_resource(data.green_texture);
	device->destroy_resource_view(data.green_texture_srv);

	device->destroy_private_data<device_data>();
}
static void on_init_cmd_list(command_list *cmd_list)
{
	cmd_list->create_private_data<cmd_data>();
}
static void on_destroy_cmd_list(command_list *cmd_list)
{
	cmd_list->destroy_private_data<cmd_data>();
}

static void on_init_texture(device *device, const resource_desc &desc, const subresource_data *, resource_usage, resource res)
{
	if (desc.type != resource_type::texture_2d)
		return;
	if (device->get_api() != device_api::opengl && (desc.usage & (resource_usage::shader_resource | resource_usage::depth_stencil | resource_usage::render_target)) != resource_usage::shader_resource)
		return;

	auto &data = device->get_private_data<device_data>();

	std::lock_guard<std::mutex> lock(s_mutex);

	data.total_texture_list.emplace(res, tex_data { desc });
}
static void on_destroy_texture(device *device, resource res)
{
	auto &data = device->get_private_data<device_data>();

	// In some cases the 'destroy_device' event may be called before all resources have been destroyed
	if (&data == nullptr)
		return;

	std::lock_guard<std::mutex> lock(s_mutex);

	if (const auto it = data.total_texture_list.find(res);
		it != data.total_texture_list.end())
	{
		// TODO: Crash will occur if application destroys texture, but it is still referenced in command list with ImGui commands being executed
		data.total_texture_list.erase(it);
	}
}
static void on_destroy_texture_view(device *device, resource_view view)
{
	auto &data = device->get_private_data<device_data>();

	// In some cases the 'destroy_device' event may be called before all resource views have been destroyed
	if (&data == nullptr)
		return;

	std::lock_guard<std::mutex> lock(s_mutex);

	data.destroyed_views.push_back(view);

	for (auto &tex : data.total_texture_list)
	{
		if (tex.second.last_view == view)
		{
			tex.second.last_view.handle = 0;
		}
	}
}

static void on_push_descriptors(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param_index, const descriptor_set_update &update)
{
	if ((stages & shader_stage::pixel) != shader_stage::pixel || (update.type != descriptor_type::shader_resource_view && update.type != descriptor_type::sampler_with_resource_view))
		return;

	device *const device = cmd_list->get_device();
	auto &data = device->get_private_data<struct device_data>();
	auto &cmd_data = cmd_list->get_private_data<struct cmd_data>();

	for (uint32_t i = 0; i < update.count; ++i)
	{
		if (update.type == descriptor_type::shader_resource_view)
		{
			resource_view descriptor = static_cast<const resource_view *>(update.descriptors)[i];
			if (descriptor.handle == 0)
				continue;

			cmd_data.current_texture_list.emplace(descriptor);

			if (data.replaced_texture_srv == descriptor)
			{
				descriptor = data.green_texture_srv;

				descriptor_set_update new_update = update;
				new_update.binding += i;
				new_update.count = 1;
				new_update.descriptors = &descriptor;

				cmd_list->push_descriptors(stages, layout, param_index, new_update);
			}
		}
		else
		{
			sampler_with_resource_view descriptor = static_cast<const sampler_with_resource_view *>(update.descriptors)[i];
			if (descriptor.view.handle == 0)
				continue;

			cmd_data.current_texture_list.emplace(descriptor.view);

			if (data.replaced_texture_srv == descriptor.view)
			{
				descriptor.view = data.green_texture_srv;

				descriptor_set_update new_update = update;
				new_update.binding += i;
				new_update.count = 1;
				new_update.descriptors = &descriptor;

				cmd_list->push_descriptors(stages, layout, param_index, new_update);
			}
		}
	}
}
static void on_bind_descriptor_sets(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_set *sets)
{
	if ((stages & shader_stage::pixel) != shader_stage::pixel)
		return;

	device *const device = cmd_list->get_device();
	auto &cmd_data = cmd_list->get_private_data<struct cmd_data>();
	auto &descriptor_data = device->get_private_data<struct descriptor_set_tracking>();
	assert((&descriptor_data) != nullptr);

	for (uint32_t i = 0; i < count; ++i)
	{
		const pipeline_layout_param param = descriptor_data.get_pipeline_layout_param(layout, first + i);
		assert(param.type == pipeline_layout_param_type::descriptor_set);
	
		for (uint32_t k = 0; k < param.descriptor_set.count; ++k)
		{
			const descriptor_range &range = param.descriptor_set.ranges[k];
	
			if ((range.visibility & shader_stage::pixel) != shader_stage::pixel || (range.type != descriptor_type::shader_resource_view && range.type != descriptor_type::sampler_with_resource_view))
				continue;

			uint32_t base_offset = 0;
			descriptor_pool pool = { 0 };
			device->get_descriptor_pool_offset(sets[i], range.binding, 0, &pool, &base_offset);

			for (uint32_t j = 0; j < std::min(10u, range.count); ++j)
			{
				resource_view descriptor = descriptor_data.get_shader_resource_view(pool, base_offset + j);
				if (descriptor.handle == 0)
					continue;

				cmd_data.current_texture_list.emplace(descriptor);
			}
		}
	}
}

static void on_execute(command_queue *, command_list *cmd_list)
{
	device *const device = cmd_list->get_device();
	auto &data = device->get_private_data<struct device_data>();
	auto &cmd_data = cmd_list->get_private_data<struct cmd_data>();

	data.current_texture_list.insert(cmd_data.current_texture_list.begin(), cmd_data.current_texture_list.end());
	cmd_data.current_texture_list.clear();
}

static void on_present(command_queue *queue, swapchain *swapchain)
{
	device *const device = swapchain->get_device();

	if (device->get_api() != device_api::d3d12 && device->get_api() != device_api::vulkan)
		on_execute(queue, queue->get_immediate_command_list());

	auto &data = device->get_private_data<device_data>();
	data.frame_index++;

	std::lock_guard<std::mutex> lock(s_mutex);

	for (auto srv : data.current_texture_list)
	{
		if (std::find(data.destroyed_views.begin(), data.destroyed_views.end(), srv) != data.destroyed_views.end())
			continue;

		resource res = device->get_resource_from_view(srv);

		if (const auto it = data.total_texture_list.find(res);
			it != data.total_texture_list.end())
		{
			tex_data &tex_data = it->second;
			tex_data.last_view = srv;
			tex_data.last_used = data.frame_index;
		}
	}

	data.current_texture_list.clear();
	data.destroyed_views.clear();
}

// See implementation in 'dump_texture.cpp'
extern bool dump_texture(const resource_desc &desc, const subresource_data &data);

static bool dump_texture(command_queue *queue, resource tex, const resource_desc &desc)
{
	device *const device = queue->get_device();

	uint32_t row_pitch = format_row_pitch(desc.texture.format, desc.texture.width);
	if (device->get_api() == device_api::d3d12) // Align row pitch to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256)
		row_pitch = (row_pitch + 255) & ~255;
	const uint32_t slice_pitch = format_slice_pitch(desc.texture.format, row_pitch, desc.texture.height);

	resource intermediate;
	if (desc.heap != memory_heap::gpu_only)
	{
		// Avoid copying to temporary system memory resource if texture is accessible directly
		intermediate = tex;
	}
	else if (device->check_capability(device_caps::copy_buffer_to_texture))
	{
		if ((desc.usage & resource_usage::copy_source) != resource_usage::copy_source)
			return false;

		if (!device->create_resource(resource_desc(slice_pitch, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &intermediate))
		{
			reshade::log_message(1, "Failed to create system memory buffer for texture dumping!");
			return false;
		}

		command_list *const cmd_list = queue->get_immediate_command_list();
		cmd_list->barrier(tex, resource_usage::shader_resource, resource_usage::copy_source);
		cmd_list->copy_texture_to_buffer(tex, 0, nullptr, intermediate, 0, desc.texture.width, desc.texture.height);
		cmd_list->barrier(tex, resource_usage::copy_source, resource_usage::shader_resource);
	}
	else
	{
		if ((desc.usage & resource_usage::copy_source) != resource_usage::copy_source)
			return false;

		if (!device->create_resource(resource_desc(desc.texture.width, desc.texture.height, 1, 1, format_to_default_typed(desc.texture.format), 1, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &intermediate))
		{
			reshade::log_message(1, "Failed to create system memory texture for texture dumping!");
			return false;
		}

		command_list *const cmd_list = queue->get_immediate_command_list();
		cmd_list->barrier(tex, resource_usage::shader_resource, resource_usage::copy_source);
		cmd_list->copy_texture_region(tex, 0, nullptr, intermediate, 0, nullptr);
		cmd_list->barrier(tex, resource_usage::copy_source, resource_usage::shader_resource);
	}

	queue->wait_idle();

	subresource_data mapped_data = {};
	if (desc.heap == memory_heap::gpu_only &&
		device->check_capability(device_caps::copy_buffer_to_texture))
	{
		device->map_buffer_region(intermediate, 0, std::numeric_limits<uint64_t>::max(), map_access::read_only, &mapped_data.data);

		mapped_data.row_pitch = row_pitch;
		mapped_data.slice_pitch = slice_pitch;
	}
	else
	{
		device->map_texture_region(intermediate, 0, nullptr, map_access::read_only, &mapped_data);
	}

	if (mapped_data.data != nullptr)
	{
		dump_texture(desc, mapped_data);

		if (desc.heap == memory_heap::gpu_only &&
			device->check_capability(device_caps::copy_buffer_to_texture))
			device->unmap_buffer_region(intermediate);
		else
			device->unmap_texture_region(intermediate, 0);
	}

	if (intermediate != tex)
		device->destroy_resource(intermediate);

	return true;
}

static void draw_overlay(effect_runtime *runtime)
{
	device *const device = runtime->get_device();
	auto &data = device->get_private_data<device_data>();

	ImGui::Checkbox("Show only used this frame", &data.filter);

	const bool save_all_textures = ImGui::Button("Save All", ImVec2(ImGui::GetWindowContentRegionWidth(), 0));

	ImGui::TextUnformatted("You can hover over a texture below with the mouse cursor to replace it with green.");
	ImGui::TextUnformatted("Clicking one will save it as an image to disk.");

	ImGui::PushItemWidth(ImGui::GetWindowContentRegionWidth());
	ImGui::SliderFloat("##scale", &data.scale, 0.01f, 2.0f, "%.3f", ImGuiSliderFlags_NoInput);
	ImGui::PopItemWidth();

	const auto total_width = ImGui::GetWindowContentRegionWidth();
	const auto num_columns = static_cast<unsigned int>(std::ceilf(total_width / (50.0f * data.scale * 13)));
	const auto single_image_max_size = (total_width / num_columns) - 5.0f;

	data.replaced_texture_srv = { 0 };

	std::lock_guard<std::mutex> lock(s_mutex);

	std::vector<std::pair<resource, tex_data>> filtered_texture_list;
	for (const auto &[tex, tex_data] : data.total_texture_list)
	{
		if (tex_data.last_view.handle == 0)
			continue;
		if (data.filter && tex_data.last_used != data.frame_index)
			continue;

		if (save_all_textures)
			dump_texture(runtime->get_command_queue(), tex, tex_data.desc);

		filtered_texture_list.emplace_back(tex, tex_data);
	}

	int texture_index = 0;

	for (size_t i = 0; i < filtered_texture_list.size(); ++i)
	{
		const tex_data &tex_data = filtered_texture_list[i].second;

		ImGui::PushID(texture_index);
		ImGui::BeginGroup();

		ImGui::Text("%ux%u", tex_data.desc.texture.width, tex_data.desc.texture.height);

		const float aspect_ratio = static_cast<float>(tex_data.desc.texture.width) / static_cast<float>(tex_data.desc.texture.height);

		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size = aspect_ratio > 1 ? ImVec2(single_image_max_size, single_image_max_size / aspect_ratio) : ImVec2(single_image_max_size * aspect_ratio, single_image_max_size);

		ImGui::Image(tex_data.last_view, size, ImVec2(0, 0), ImVec2(1, 1), ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y)) ? ImColor(0.0f, 1.0f, 0.0f) : ImColor(1.0f, 1.0f, 1.0f), ImColor(0.0f, 0.0f, 0.0f, 0.0f));

		if (ImGui::IsItemHovered())
			data.replaced_texture_srv = tex_data.last_view;

		if (ImGui::IsItemClicked())
			dump_texture(runtime->get_command_queue(), filtered_texture_list[i].first, tex_data.desc);

		if (aspect_ratio < 1)
		{
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::Dummy(ImVec2(single_image_max_size * (1 - aspect_ratio), single_image_max_size));
		}

		ImGui::EndGroup();
		ImGui::PopID();

		if ((texture_index++ % num_columns) != (num_columns - 1))
			ImGui::SameLine(0.0f, 5.0f);
		else
			ImGui::Spacing();
	}

	if ((texture_index % num_columns) != 0)
		ImGui::NewLine(); // Reset ImGui::SameLine() so the following starts on a new line
}

void register_addon_texmod_overlay()
{
	reshade::register_overlay("TexMod", draw_overlay);

	reshade::register_event<reshade::addon_event::init_device>(on_init_device);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::register_event<reshade::addon_event::init_command_list>(on_init_cmd_list);
	reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_cmd_list);

	reshade::register_event<reshade::addon_event::init_resource>(on_init_texture);
	reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_texture);
	reshade::register_event<reshade::addon_event::destroy_resource_view>(on_destroy_texture_view);

	reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::register_event<reshade::addon_event::bind_descriptor_sets>(on_bind_descriptor_sets);

	reshade::register_event<reshade::addon_event::execute_command_list>(on_execute);
	reshade::register_event<reshade::addon_event::present>(on_present);
}
void unregister_addon_texmod_overlay()
{
	reshade::unregister_overlay("TexMod", draw_overlay);

	reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
	reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::unregister_event<reshade::addon_event::init_command_list>(on_init_cmd_list);
	reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_cmd_list);

	reshade::unregister_event<reshade::addon_event::init_resource>(on_init_texture);
	reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_texture);
	reshade::unregister_event<reshade::addon_event::destroy_resource_view>(on_destroy_texture_view);

	reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::unregister_event<reshade::addon_event::bind_descriptor_sets>(on_bind_descriptor_sets);

	reshade::unregister_event<reshade::addon_event::execute_command_list>(on_execute);
	reshade::unregister_event<reshade::addon_event::present>(on_present);
}

extern "C" __declspec(dllexport) const char *NAME = "TextureMod Overlay";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Example add-on that shows an overlay to inspect the textures used by the application.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		register_addon_texmod_overlay();
		register_descriptor_set_tracking();
		break;
	case DLL_PROCESS_DETACH:
		unregister_addon_texmod_overlay();
		unregister_descriptor_set_tracking();
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
