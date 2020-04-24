#pragma once

namespace phi::sc
{
struct binary;
struct compiler;

void write_binary_to_file(phi::sc::binary const& binary, char const* path, char const* ending);

/// compile a shader and directly write both target versions to file, returns true on success
bool compile_shader(phi::sc::compiler& compiler, char const* arg_pathin, char const* arg_target, char const* arg_entrypoint, char const* arg_pathout);
}
