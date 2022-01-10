/*
 * Copyright (C) 2021 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include <reshade.hpp>
#include "crc32_hash.hpp"
#include <fstream>
#include <filesystem>

using namespace reshade::api;

static thread_local std::vector<std::vector<uint8_t>> data_to_delete;

static bool replace_shader_code(device_api device_type, shader_stage, shader_desc &desc)
{
	if (desc.code_size == 0)
		return false;

	uint32_t shader_hash = compute_crc32(static_cast<const uint8_t *>(desc.code), desc.code_size);

	const wchar_t *extension = L".cso";
	if (device_type == device_api::vulkan || (
		device_type == device_api::opengl && desc.code_size > sizeof(uint32_t) && *static_cast<const uint32_t *>(desc.code) == 0x07230203 /* SPIR-V magic */))
		extension = L".spv"; // Vulkan uses SPIR-V (and sometimes OpenGL does too)
	else if (device_type == device_api::opengl)
		extension = L".glsl"; // OpenGL otherwise uses plain text GLSL

	// Prepend executable file name to image files
	WCHAR file_prefix[MAX_PATH] = L"";
	GetModuleFileNameW(nullptr, file_prefix, ARRAYSIZE(file_prefix));

	char hash_string[11];
	sprintf_s(hash_string, "0x%08X", shader_hash);

	std::filesystem::path replace_path = file_prefix;
	replace_path += L'_';
	replace_path += L"shader_";
	replace_path += hash_string;
	replace_path += extension;

	// Check if a replacement file for this shader hash exists and if so, overwrite the shader code with its contents
	if (std::filesystem::exists(replace_path))
	{
		std::ifstream file(replace_path, std::ios::binary);
		file.seekg(0, std::ios::end);
		std::vector<uint8_t> shader_code(static_cast<size_t>(file.tellg()));
		file.seekg(0, std::ios::beg).read(reinterpret_cast<char *>(shader_code.data()), shader_code.size());

		data_to_delete.push_back(std::move(shader_code));

		desc.code = data_to_delete.back().data();
		desc.code_size = data_to_delete.back().size();
		return true;
	}

	return false;
}

static bool on_create_pipeline(device *device, pipeline_desc &desc, uint32_t, const dynamic_state *)
{
	bool replaced_stages = false;
	const device_api device_type = device->get_api();

	// Go through all shader stages that are in this pipeline and potentially replace the associated shader code
	if ((desc.type & pipeline_stage::vertex_shader) != 0)
		replaced_stages |= replace_shader_code(device_type, shader_stage::vertex, desc.graphics.vertex_shader);
	if ((desc.type & pipeline_stage::hull_shader) != 0)
		replaced_stages |= replace_shader_code(device_type, shader_stage::hull, desc.graphics.hull_shader);
	if ((desc.type & pipeline_stage::domain_shader) != 0)
		replaced_stages |= replace_shader_code(device_type, shader_stage::domain, desc.graphics.domain_shader);
	if ((desc.type & pipeline_stage::geometry_shader) != 0)
		replaced_stages |= replace_shader_code(device_type, shader_stage::geometry, desc.graphics.geometry_shader);
	if ((desc.type & pipeline_stage::pixel_shader) != 0)
		replaced_stages |= replace_shader_code(device_type, shader_stage::pixel, desc.graphics.pixel_shader);
	if ((desc.type & pipeline_stage::compute_shader) != 0)
		replaced_stages |= replace_shader_code(device_type, shader_stage::compute, desc.compute.shader);

	// Return whether any shader code was replaced
	return replaced_stages;
}
static void on_after_create_pipeline(device *, const pipeline_desc &, uint32_t, const dynamic_state *, pipeline)
{
	// Free the memory allocated in 'replace_shader_code' above
	data_to_delete.clear();
}

extern "C" __declspec(dllexport) const char *NAME = "Shader Replace";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Example add-on that replaces shaders the application creates with binaries loaded from disk.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::register_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_after_create_pipeline);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
