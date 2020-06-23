#pragma once

namespace dxcw
{
struct binary;
struct compiler;
struct shaderlist_compilation_result;
struct shaderlist_entry_owning;
struct include_entry;

/// Writes a compiled binary to disk, creates folders if nonexisting
bool write_binary_to_file(dxcw::binary const& binary, char const* path, char const* ending);

/// compile a shader and directly write both target versions to file, returns true on success
/// output_path without file ending
///
/// Usage:
/// compile_shader(comp, "res/shader.hlsl", "vs", "main_vertex", "res/bin/shader_vs");
bool compile_shader(dxcw::compiler& compiler, char const* source_path, char const* shader_target, char const* entrypoint, char const* output_path, char const* optional_include_dir = nullptr);

/// compile and write to disk all shaders as specified in a shaderlist.txt file
///
/// returns amount of compiled shaders, -1 if the shaderlist cannot be opened
/// out_num_errors optionally receives amount of parse and compile errors
///
/// shaderlist file: ASCII, line-by-line, lines empty or starting with # are ignored
/// paths relative to path of shaderlist file
/// example:
///
/// # [input file] [entrypoint] [type (vs/ps/gs/ds/hs/cs)] [output file without extension]
/// # imgui
/// src/imgui.hlsl main_vs vs bin/imgui_vs
/// src/imgui.hlsl main_ps ps bin/imgui_ps
void compile_shaderlist(dxcw::compiler& compiler, char const* shaderlist_file, shaderlist_compilation_result* out_results = nullptr);

struct shaderlist_compilation_result
{
    int num_shaders_detected;
    int num_errors;
};

/// parse a shaderlist and write its entries to an array, no I/O writes
/// returns amount of entries written
/// if the return value is > max_num_out, more entries could have been written
/// out_entries can be null
unsigned parse_shaderlist(char const* shaderlist_file, shaderlist_entry_owning* out_entries, unsigned max_num_out);

struct shaderlist_entry_owning
{
    char pathin[1024];
    char pathin_absolute[1024];
    char pathout_absolute[1024];
    char target[4];
    char entrypoint[64];
};

unsigned parse_includes(char const* source_path, char const* include_path, include_entry* out_include_entries, unsigned max_num_out);

struct include_entry
{
    char includepath_absolute[1024];
};
}
