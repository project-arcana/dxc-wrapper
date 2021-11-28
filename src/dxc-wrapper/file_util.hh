#pragma once

#include <cstdint>

#include <clean-core/fwd.hh>

#include <dxc-wrapper/common/api.hh>
#include <dxc-wrapper/fwd.hh>

namespace dxcw
{
struct shaderlist_compilation_result;
struct shaderlist_binary_entry_owning;
struct shaderlist_library_entry_owning;
struct include_entry;
struct fixed_string;

/// Parses the target from a string, ie "vs" -> dxcw::target::vertex
DXCW_API bool parse_target(char const* str, dxcw::target& out_tgt);

/// Writes a compiled binary to disk, creates folders if nonexisting
DXCW_API bool write_binary_to_file(dxcw::binary const& binary, char const* path, char const* ending);

DXCW_API bool write_binary_to_file(dxcw::binary const& binary, char const* path);

/// compile a shader and directly write both target versions to file, returns true on success
/// output_path without file ending
///
/// Usage:
/// compile_shader(comp, "res/shader.hlsl", "vs", "main_vertex", "res/bin/shader_vs");
DXCW_API bool compile_shader(dxcw::compiler& compiler,
                             char const* source_path,
                             char const* shader_target,
                             char const* entrypoint,
                             char const* output_path,
                             cc::span<char const* const> opt_additional_include_paths = {},
                             cc::allocator* scratch_alloc = cc::system_allocator);

DXCW_API bool compile_library(dxcw::compiler& compiler,
                              char const* source_path,
                              cc::span<library_export const> exports,
                              char const* output_path,
                              cc::span<char const* const> opt_additional_include_paths = {},
                              cc::allocator* scratch_alloc = cc::system_allocator);

DXCW_API bool compile_binary_entry(compiler& compiler,
                                   dxcw::shaderlist_binary_entry_owning const& entry,
                                   cc::span<char const* const> opt_additional_include_paths,
                                   cc::allocator* scratch_alloc);

DXCW_API bool compile_library_entry(compiler& compiler,
                                    dxcw::shaderlist_library_entry_owning const& entry,
                                    cc::span<char const* const> opt_additional_include_paths,
                                    cc::allocator* scratch_alloc);

/// compile and write to disk all shaders as specified in a shaderlist.txt file
///
/// returns false if the shaderlist cannot be opened
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
DXCW_API bool compile_shaderlist(dxcw::compiler& compiler,
                                 char const* shaderlist_file,
                                 shaderlist_compilation_result* out_results = nullptr,
                                 cc::allocator* scratch_alloc = cc::system_allocator);

/// compile and write to disk all shaders as specified in a json file
///
/// returns false if the shaderlist cannot be opened or parsed
/// out_num_errors optionally receives amount of non-fatal parse and compile errors
///
/// shaderlist file: JSON array of objects
DXCW_API bool compile_shaderlist_json(dxcw::compiler& compiler,
                                      char const* json_file,
                                      shaderlist_compilation_result* out_results = nullptr,
                                      cc::allocator* scratch_alloc = cc::system_allocator);

struct shaderlist_compilation_result
{
    int num_shaders_detected;
    int num_libraries_detected;
    int num_errors;
};

/// parse a shaderlist and write its entries to an array, no I/O writes
/// returns amount of entries written
/// if the return value is > max_num_out, more entries could have been written
/// out_entries can be null
DXCW_API unsigned parse_shaderlist(char const* shaderlist_file, shaderlist_binary_entry_owning* out_entries, unsigned max_num_out);

DXCW_API bool parse_shaderlist_json(char const* shaderlist_file,
                                    cc::span<shaderlist_binary_entry_owning> out_binaries,
                                    unsigned& out_num_binaries,
                                    cc::span<shaderlist_library_entry_owning> out_libraries,
                                    unsigned& out_num_libraries,
                                    cc::allocator* scratch_alloc = cc::system_allocator);


/// recursively parses all #include directories, resolves them to absolute paths, and returns a unique list
DXCW_API cc::alloc_vector<fixed_string> parse_includes(char const* source_path, cc::span<char const* const> include_paths, cc::allocator* allocator = cc::system_allocator);


struct shaderlist_binary_entry_owning
{
    char pathin[1024];
    char pathin_absolute[1024];
    char pathout_absolute[1024];
    char target[4];
    char entrypoint[64];
};

struct shaderlist_library_entry_owning
{
    char pathin[1024];
    char pathin_absolute[1024];
    char pathout_absolute[1024];
    char entrypoint_buffer[8192];
    char const* exports_internal_names[32]; // point into entrypoint_buffer
    char const* exports_exported_names[32]; // point into entrypoint_buffer or nullptr
    uint8_t num_exports;
};

struct fixed_string
{
    char str[512];
};
}
