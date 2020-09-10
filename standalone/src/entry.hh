#pragma once

#include <clean-core/fwd.hh>

#include <nexus/fwd.hh>

namespace dxcw
{
int compile_shader_single(nx::args const& args);

int compile_shaderlist_single(char const* shaderlist_path);

int compile_shaderlist_watch(char const* shaderlist_path, cc::allocator* scratch_alloc = cc::system_allocator);

int compile_shaderlist_json_single(char const* shaderlist_json, cc::allocator *scratch_alloc = cc::system_allocator);

int compile_shaderlist_json_watch(char const* shaderlist_json, cc::allocator* scratch_alloc = cc::system_allocator);
}
