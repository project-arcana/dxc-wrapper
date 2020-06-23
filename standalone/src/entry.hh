#pragma once

#include <nexus/fwd.hh>

namespace dxcw
{
int compile_shader_single(nx::args const& args);

int compile_shaderlist_single(char const* shaderlist_path);

int compile_shaderlist_watch(char const* shaderlist_path);
}
