#include "file_util.hh"

#include <cstdio>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

#include <clean-core/alloc_array.hh>
#include <clean-core/assert.hh>

#include <dxc-wrapper/common/log.hh>
#include <dxc-wrapper/common/tinyjson.hh>
#include <dxc-wrapper/compiler.hh>

// use this over std::strncpy wherever possible (direct reference to dest buffer)
#ifdef CC_OS_WINDOWS
#define DXCW_STRNCPY ::strncpy_s
#else
#define DXCW_STRNCPY std::strncpy
#endif

namespace
{
cc::alloc_array<char> read_file(char const* path, cc::allocator* alloc)
{
#ifdef CC_OS_WINDOWS
    std::FILE* fp = nullptr;
    errno_t err = ::fopen_s(&fp, path, "rb");
    if (err != 0 || !fp)
        return {};
#else
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp)
        return {};
#endif

    std::fseek(fp, 0, SEEK_END);
    auto res = cc::alloc_array<char>::uninitialized(std::ftell(fp) + 1, alloc);
    std::rewind(fp);
    std::fread(&res[0], 1, res.size() - 1, fp);
    std::fclose(fp);

    res[res.size() - 1] = '\0';
    return res;
}

bool parse_target(char const* str, dxcw::target& out_tgt)
{
    if (std::strlen(str) < 1)
    {
        return false;
    }

    // first char is enough
    switch (str[0])
    {
    case 'v':
        out_tgt = dxcw::target::vertex;
        return true;
    case 'h':
        out_tgt = dxcw::target::hull;
        return true;
    case 'd':
        out_tgt = dxcw::target::domain;
        return true;
    case 'g':
        out_tgt = dxcw::target::geometry;
        return true;
    case 'p':
        out_tgt = dxcw::target::pixel;
        return true;
    case 'c':
        out_tgt = dxcw::target::compute;
        return true;
    default:
        return false;
    }
}
}

bool dxcw::write_binary_to_file(const dxcw::binary& binary, const char* path, const char* ending)
{
    if (binary.data == nullptr)
        return false;

    char outpath[1024];
    std::snprintf(outpath, sizeof(outpath), "%s.%s", path, ending);

    // recursively create directories required for the output
    std::filesystem::create_directories(std::filesystem::path(outpath).remove_filename());

    auto outfile = std::fstream(outpath, std::ios::out | std::ios::binary);

    if (!outfile.good())
    {
        DXCW_LOG_ERROR("failed to write shader to {}.{}", path, ending);
        return false;
    }

    outfile.write(reinterpret_cast<char const*>(binary.data), std::streamsize(binary.size));
    outfile.close();
    return true;
}

bool dxcw::compile_shader(dxcw::compiler& compiler,
                          const char* source_path,
                          const char* shader_target,
                          const char* entrypoint,
                          const char* output_path,
                          const char* optional_include_dir,
                          cc::allocator* scratch_alloc)
{
    auto const content = read_file(source_path, scratch_alloc);

    if (content.empty())
    {
        DXCW_LOG_ERROR("failed to open shader source file at {}", source_path);
        return false;
    }

    dxcw::target parsed_target;
    if (!parse_target(shader_target, parsed_target))
    {
        return false;
    }

#ifdef CC_OS_WINDOWS
    auto dxil_binary = compiler.compile_shader(content.data(), entrypoint, parsed_target, dxcw::output::dxil, false, optional_include_dir,
                                               source_path, nullptr, scratch_alloc);

    if (dxil_binary.internal_blob == nullptr)
        return false;

    dxcw::write_binary_to_file(dxil_binary, output_path, "dxil");
    dxcw::destroy_blob(dxil_binary.internal_blob);
#endif
    // On non-windows, DXIL can be compiled but not signed which makes it mostly useless
    // requiring DXIL on linux would be a pretty strange path but can be supported with more tricks

    auto spv_binary = compiler.compile_shader(content.data(), entrypoint, parsed_target, dxcw::output::spirv, false, optional_include_dir,
                                              source_path, nullptr, scratch_alloc);
    if (spv_binary.internal_blob == nullptr)
        return false;

    dxcw::write_binary_to_file(spv_binary, output_path, "spv");
    dxcw::destroy_blob(spv_binary.internal_blob);
    return true;
}

bool dxcw::compile_library(dxcw::compiler& compiler,
                           const char* source_path,
                           cc::span<const library_export> exports,
                           const char* output_path,
                           const char* optional_include_dir,
                           cc::allocator* scratch_alloc)
{
    if (exports.empty())
    {
        DXCW_LOG_WARN("skipping compilation of library without exports at {}", source_path);
        return false;
    }

    auto const content = read_file(source_path, scratch_alloc);

    if (content.empty())
    {
        DXCW_LOG_ERROR("failed to open library source file at {}", source_path);
        return false;
    }

#ifdef CC_OS_WINDOWS
    auto dxil_binary = compiler.compile_library(content.data(), exports, dxcw::output::dxil, false, optional_include_dir, source_path, nullptr, scratch_alloc);

    if (dxil_binary.internal_blob == nullptr)
        return false;

    dxcw::write_binary_to_file(dxil_binary, output_path, "dxil");
    dxcw::destroy_blob(dxil_binary.internal_blob);
#endif
    // On non-windows, DXIL can be compiled but not signed which makes it mostly useless
    // requiring DXIL on linux would be a pretty strange path but can be supported with more tricks

    auto spv_binary = compiler.compile_library(content.data(), exports, dxcw::output::spirv, false, optional_include_dir, source_path, nullptr, scratch_alloc);
    if (spv_binary.internal_blob == nullptr)
        return false;

    dxcw::write_binary_to_file(spv_binary, output_path, "spv");
    dxcw::destroy_blob(spv_binary.internal_blob);
    return true;
}


bool dxcw::compile_binary_entry(dxcw::compiler& compiler, const dxcw::shaderlist_binary_entry_owning& entry, const char* opt_include_dir, cc::allocator* scratch_alloc)
{
    auto const success
        = dxcw::compile_shader(compiler, entry.pathin_absolute, entry.target, entry.entrypoint, entry.pathout_absolute, opt_include_dir, scratch_alloc);

    if (success)
        DXCW_LOG("compiled {} ({}; {})", entry.pathin, entry.target, entry.entrypoint);
    else
        DXCW_LOG_WARN("error compiling {} ({}; {})", entry.pathin, entry.target, entry.entrypoint);

    return success;
}

bool dxcw::compile_library_entry(dxcw::compiler& compiler, const dxcw::shaderlist_library_entry_owning& entry, const char* opt_include_dir, cc::allocator* scratch_alloc)
{
    auto exports = cc::alloc_array<library_export>::uninitialized(entry.num_exports, scratch_alloc);

    for (auto i = 0u; i < entry.num_exports; ++i)
    {
        exports[i].internal_name = entry.exports_internal_names[i];
        exports[i].export_name = entry.exports_exported_names[i];
    }

    auto const success = dxcw::compile_library(compiler, entry.pathin_absolute, exports, entry.pathout_absolute, opt_include_dir, scratch_alloc);

    if (success)
        DXCW_LOG("compiled library {} ({} exports)", entry.pathin, entry.num_exports);
    else
        DXCW_LOG_WARN("error compiling library {} ({} exports)", entry.pathin, entry.num_exports);

    return success;
}


bool dxcw::compile_shaderlist(dxcw::compiler& compiler, const char* shaderlist_file, shaderlist_compilation_result* out_results, cc::allocator* scratch_alloc)
{
    auto const f_onerror = [&]() -> void {
        DXCW_LOG_ERROR("failed to open shaderlist file at {}", shaderlist_file);
        if (out_results)
            *out_results = {-1, -1, 1};
    };


    std::ifstream in_file(shaderlist_file);
    if (!in_file.good())
    {
        f_onerror();
        return false;
    }

    // set the working directory to the folder containing the list this was invoked with
    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_file).remove_filename(), ec);
    if (ec)
    {
        f_onerror();
        return false;
    }

    auto const base_path_string = base_path_fs.string();

    std::string line;

    std::string pathin;
    std::string target;
    std::string entrypoint;
    std::string pathout;

    auto num_lines = 0;
    auto num_errors = 0;
    auto num_shaders = 0;

    while (std::getline(in_file, line))
    {
        ++num_lines;
        // skip empty lines and comments
        if (line.empty() || line[0] == '#')
            continue;

        std::stringstream ss(line);

        if (ss >> pathin && ss >> entrypoint && ss >> target && ss >> pathout)
        {
            ec = {};
            auto const pathin_absolute = std::filesystem::canonical(base_path_fs / pathin, ec);
            if (ec)
            {
                DXCW_LOG_WARN("shader {} not found (shaderlist line {})", pathin.c_str(), num_lines);
                ++num_errors;
                continue;
            }

            ++num_shaders;
            auto const success = compile_shader(compiler, pathin_absolute.string().c_str(), target.c_str(), entrypoint.c_str(),
                                                (base_path_fs / pathout).string().c_str(), base_path_string.c_str(), scratch_alloc);

            if (!success)
                ++num_errors;
        }
        else
        {
            DXCW_LOG_WARN("failed to parse shaderlist line {}:", num_lines);
            DXCW_LOG_WARN("\"{}\"", line.c_str());
            ++num_errors;
        }
    }

    if (out_results)
        *out_results = {num_shaders, 0, num_errors};

    return true;
}

bool dxcw::compile_shaderlist_json(dxcw::compiler& compiler, const char* json_file, dxcw::shaderlist_compilation_result* out_results, cc::allocator* scratch_alloc)
{
    auto content = read_file(json_file, scratch_alloc);
    if (content.empty())
    {
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", json_file);
        return false;
    }

    // set the working directory to the folder containing the list this was invoked with
    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(json_file).remove_filename(), ec);
    if (ec)
    {
        DXCW_LOG_ERROR("failed to make path canonical for shaderlist json file at {}", json_file);
        return false;
    }

    auto const base_path_string = base_path_fs.string();
    cc::alloc_array<json_t> json_nodes(512, scratch_alloc);

    json_t const* const j_root = json_create(content.data(), json_nodes.data(), unsigned(json_nodes.size()));
    if (j_root == nullptr || j_root->type != JSON_OBJ)
    {
        DXCW_LOG_ERROR("fatal json parse error in shaderlist json {} ({} chars)", json_file, content.size());
        return false;
    }

    json_t const* j_entries_arr = json_getProperty(j_root, "entries");
    unsigned num_valid_entries = 0;
    unsigned num_entries = 0;
    int num_shaders = 0;
    int num_libraries = 0;
    int num_errors = 0;

    auto const f_get_string_prop = [](json_t const* node, char const* property_name) -> char const* {
        if (!node)
            return nullptr;
        auto const prop = json_getProperty(node, property_name);
        if (!prop || prop->type != JSON_TEXT)
            return nullptr;
        return json_getValue(prop);
    };

    if (j_entries_arr == nullptr)
    {
        DXCW_LOG_ERROR("missing root property \"entries\"");
        goto l_parse_error;
    }

    if (json_getType(j_entries_arr) != JSON_ARRAY)
    {
        DXCW_LOG_ERROR("root property \"entries\" must be an array");
        goto l_parse_error;
    }

    // loop over all entry nodes in "entries": [ ... ] array
    for (json_t const* j_entry = json_getChild(j_entries_arr); j_entry; j_entry = json_getSibling(j_entry))
    {
        ++num_entries;
        if (json_getType(j_entry) != JSON_OBJ)
        {
            DXCW_LOG_WARN("skipping non-object element #{} in \"entries\" array", num_entries);
            continue;
        }

        char const* const str_source = f_get_string_prop(j_entry, "source");
        if (!str_source)
        {
            DXCW_LOG_WARN("skipping entry #{} without required \"source\" property", num_entries);
            continue;
        }

        // "canonize" path - ie. make it absolute and check if the file exists
        ec = {};
        auto const pathin_absolute = std::filesystem::canonical(base_path_fs / str_source, ec);
        if (ec)
        {
            DXCW_LOG_WARN("shader source {} not found (shader json entry #{})", str_source, num_entries);
            ++num_errors;
            continue;
        }

        ++num_valid_entries;

        json_t const* const jp_binaries = json_getProperty(j_entry, "binaries");
        if (jp_binaries)
        {
            if (json_getType(jp_binaries) != JSON_ARRAY)
            {
                DXCW_LOG_WARN("entry #{} property \"binaries\" is not an array", num_entries);
            }
            else
            {
                unsigned num_bins = 0;
                for (json_t const* j_bin = json_getChild(jp_binaries); j_bin; j_bin = json_getSibling(j_bin))
                {
                    ++num_bins;
                    char const* const str_target = f_get_string_prop(j_bin, "target");
                    char const* const str_entrypoint = f_get_string_prop(j_bin, "entrypoint");
                    char const* const str_output = f_get_string_prop(j_bin, "output");
                    if (!str_target || !str_entrypoint || !str_output)
                    {
                        DXCW_LOG_WARN("skipping binary #{} on entry #{} without required \"target\", \"entrypoint\", or \"output\" text properties",
                                      num_bins, num_entries);
                        continue;
                    }

                    // all good, compile the shader binary
                    ++num_shaders;
                    auto const success = compile_shader(compiler, pathin_absolute.string().c_str(), str_target, str_entrypoint, str_output,
                                                        base_path_string.c_str(), scratch_alloc);

                    if (!success)
                        ++num_errors;
                }
            }
        }

        json_t const* const jp_library = json_getProperty(j_entry, "library");
        if (jp_library)
        {
            if (json_getType(jp_library) != JSON_OBJ)
            {
                DXCW_LOG_WARN("entry #{} property \"library\" is not an object", num_entries);
            }
            else
            {
                char const* const str_output = f_get_string_prop(jp_library, "output");
                if (!str_output)
                {
                    DXCW_LOG_WARN("skipping library of entry #{} which lacks required \"output\" text property", num_entries);
                    continue;
                }

                json_t const* const jp_exports = json_getProperty(jp_library, "exports");
                if (!jp_exports)
                {
                    DXCW_LOG_WARN(R"(skipping library of entry #{} which lacks required "exports" array property)", num_entries);
                    continue;
                }

                if (jp_exports->type != JSON_ARRAY)
                {
                    DXCW_LOG_WARN(R"("exports" property in library of entry #{} is not an array)", num_entries);
                    continue;
                }

                // determine the total amount of exports
                unsigned num_exports = 0;
                for (json_t const* j_exp = json_getChild(jp_exports); j_exp; j_exp = json_getSibling(j_exp))
                {
                    if (j_exp->type != JSON_TEXT && j_exp->type != JSON_OBJ)
                    {
                        DXCW_LOG_WARN(R"(an export element in library of entry #{} is neither string nor object)", num_entries);
                        continue;
                    }

                    ++num_exports;
                }

                if (num_exports == 0)
                {
                    DXCW_LOG_WARN("skipping library of entry #{} which specifies no exports", num_entries);
                    continue;
                }

                // allocate and extract entries
                auto export_array = cc::alloc_array<dxcw::library_export>::uninitialized(num_exports, scratch_alloc);
                unsigned export_array_cursor = 0;

                auto const f_add_export = [&](char const* internal_name, char const* exported_name) {
                    export_array[export_array_cursor] = library_export{internal_name, exported_name};
                    ++export_array_cursor;
                };

                for (json_t const* j_exp = json_getChild(jp_exports); j_exp; j_exp = json_getSibling(j_exp))
                {
                    if (j_exp->type != JSON_TEXT && j_exp->type != JSON_OBJ)
                    {
                        continue;
                    }

                    if (j_exp->type == JSON_TEXT)
                    {
                        f_add_export(j_exp->u.value, nullptr);
                    }
                    else if (j_exp->type == JSON_OBJ)
                    {
                        char const* str_internal = f_get_string_prop(j_exp, "internal");
                        char const* str_export = f_get_string_prop(j_exp, "export");

                        if (!str_internal)
                        {
                            DXCW_LOG_WARN(R"(an export element in library of entry #{} does not specify the required "internal" property - name of the export in HLSL)",
                                          num_entries);
                            continue;
                        }

                        f_add_export(str_internal, str_export);
                    }
                }

                // all good, compile the library
                ++num_libraries;
                auto const success = compile_library(compiler, pathin_absolute.string().c_str(), export_array,
                                                     (base_path_fs / str_output).string().c_str(), base_path_string.c_str(), scratch_alloc);

                if (!success)
                    ++num_errors;
            }
        }
    }

    if (out_results)
    {
        *out_results = {num_shaders, num_libraries, num_errors};
    }
    return true;

l_parse_error:
    DXCW_LOG_ERROR("parse error in json shader list {}", json_file);
    return false;
}

unsigned dxcw::parse_shaderlist(const char* shaderlist_file, dxcw::shaderlist_binary_entry_owning* out_entries, unsigned max_num_out)
{
    std::fstream in_file(shaderlist_file);
    if (!in_file.good())
    {
        DXCW_LOG_ERROR("failed to open shaderlist file at {}", shaderlist_file);
        return unsigned(-1);
    }

    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_file).remove_filename(), ec);
    if (ec)
    {
        DXCW_LOG_ERROR("failed to open shaderlist file at {}", shaderlist_file);
        return unsigned(-1);
    }

    std::string line;

    std::string pathin;
    std::string target;
    std::string entrypoint;
    std::string pathout;

    unsigned num_lines = 0;
    unsigned num_errors = 0;
    unsigned num_shaders = 0;

    if (out_entries == nullptr)
        max_num_out = 0;

    while (std::getline(in_file, line))
    {
        ++num_lines;
        // skip empty lines and comments
        if (line.empty() || line[0] == '#')
            continue;

        std::stringstream ss(line);

        if (ss >> pathin && ss >> entrypoint && ss >> target && ss >> pathout)
        {
            ec.clear();
            auto const pathin_absolute_fs = std::filesystem::canonical(base_path_fs / pathin, ec);
            if (ec)
            {
                DXCW_LOG_WARN("shader {} not found (shaderlist line {})", pathin.c_str(), num_lines);
                ++num_errors;
                continue;
            }

            if (num_shaders < max_num_out)
            {
                auto& write_entry = out_entries[num_shaders];
                auto const pathout_absolute = (base_path_fs / pathout).string();
                DXCW_STRNCPY(write_entry.pathin, pathin.c_str(), sizeof(write_entry.pathin));
                DXCW_STRNCPY(write_entry.pathin_absolute, pathin_absolute_fs.string().c_str(), sizeof(write_entry.pathin_absolute));
                DXCW_STRNCPY(write_entry.pathout_absolute, pathout_absolute.c_str(), sizeof(write_entry.pathout_absolute));
                DXCW_STRNCPY(write_entry.target, target.c_str(), sizeof(write_entry.target));
                DXCW_STRNCPY(write_entry.entrypoint, entrypoint.c_str(), sizeof(write_entry.entrypoint));
            }

            ++num_shaders;
        }
        else
        {
            DXCW_LOG_WARN("failed to parse shaderlist line {}:", num_lines);
            DXCW_LOG_WARN("\"{}\"", line.c_str());
            ++num_errors;
        }
    }

    return num_shaders;
}

bool dxcw::parse_shaderlist_json(const char* shaderlist_file,
                                 cc::span<dxcw::shaderlist_binary_entry_owning> out_binaries,
                                 unsigned& out_num_binaries,
                                 cc::span<dxcw::shaderlist_library_entry_owning> out_libraries,
                                 unsigned& out_num_libraries,
                                 cc::allocator* scratch_alloc)
{
    auto content = read_file(shaderlist_file, scratch_alloc);
    if (content.empty())
    {
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", shaderlist_file);
        return false;
    }

    // set the working directory to the folder containing the list this was invoked with
    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_file).remove_filename(), ec);
    if (ec)
    {
        DXCW_LOG_ERROR("failed to make path canonical for shaderlist json file at {}", shaderlist_file);
        return false;
    }

    auto const base_path_string = base_path_fs.string();
    cc::alloc_array<json_t> json_nodes(512, scratch_alloc);

    json_t const* const j_root = json_create(content.data(), json_nodes.data(), unsigned(json_nodes.size()));
    if (j_root == nullptr || j_root->type != JSON_OBJ)
    {
        DXCW_LOG_ERROR("fatal json parse error in shaderlist json {} ({} chars)", shaderlist_file, content.size());
        return false;
    }

    json_t const* j_entries_arr = json_getProperty(j_root, "entries");
    unsigned num_valid_entries = 0;
    unsigned num_entries = 0;
    out_num_binaries = 0;
    out_num_libraries = 0;
    int num_errors = 0;

    auto const f_get_string_prop = [](json_t const* node, char const* property_name) -> char const* {
        if (!node)
            return nullptr;
        auto const prop = json_getProperty(node, property_name);
        if (!prop || prop->type != JSON_TEXT)
            return nullptr;
        return json_getValue(prop);
    };

    if (j_entries_arr == nullptr)
    {
        DXCW_LOG_ERROR("missing root property \"entries\"");
        goto l_parse_error;
    }
    if (json_getType(j_entries_arr) != JSON_ARRAY)
    {
        DXCW_LOG_ERROR("root property \"entries\" must be an array");
        goto l_parse_error;
    }

    // loop over all entry nodes in "entries": [ ... ] array
    for (json_t const* j_entry = json_getChild(j_entries_arr); j_entry; j_entry = json_getSibling(j_entry))
    {
        ++num_entries;
        if (json_getType(j_entry) != JSON_OBJ)
        {
            DXCW_LOG_WARN("skipping non-object element #{} in \"entries\" array", num_entries);
            ++num_errors;
            continue;
        }

        char const* const str_source = f_get_string_prop(j_entry, "source");
        if (!str_source)
        {
            DXCW_LOG_WARN("skipping entry #{} without required \"source\" property", num_entries);
            ++num_errors;
            continue;
        }

        // "canonize" path - ie. make it absolute and check if the file exists
        ec = {};
        auto const pathin_absolute = std::filesystem::canonical(base_path_fs / str_source, ec);
        if (ec)
        {
            DXCW_LOG_WARN("shader source {} not found (shader json entry #{})", str_source, num_entries);
            ++num_errors;
            continue;
        }

        ++num_valid_entries;

        json_t const* const jp_binaries = json_getProperty(j_entry, "binaries");
        if (jp_binaries)
        {
            if (json_getType(jp_binaries) != JSON_ARRAY)
            {
                DXCW_LOG_WARN("entry #{} property \"binaries\" is not an array", num_entries);
                ++num_errors;
            }
            else
            {
                unsigned num_bins = 0;
                for (json_t const* j_bin = json_getChild(jp_binaries); j_bin; j_bin = json_getSibling(j_bin))
                {
                    ++num_bins;
                    char const* const str_target = f_get_string_prop(j_bin, "target");
                    char const* const str_entrypoint = f_get_string_prop(j_bin, "entrypoint");
                    char const* const str_output = f_get_string_prop(j_bin, "output");
                    if (!str_target || !str_entrypoint || !str_output)
                    {
                        DXCW_LOG_WARN("skipping binary #{} on entry #{} without required \"target\", \"entrypoint\", or \"output\" text properties",
                                      num_bins, num_entries);
                        ++num_errors;
                        continue;
                    }

                    // all good, write the shader binary info
                    if (out_num_binaries < out_binaries.size())
                    {
                        auto& write_entry = out_binaries[out_num_binaries];
                        auto const pathout_absolute = (base_path_fs / str_output).string();

                        DXCW_STRNCPY(write_entry.pathin, str_source, sizeof(write_entry.pathin));
                        DXCW_STRNCPY(write_entry.pathin_absolute, pathin_absolute.string().c_str(), sizeof(write_entry.pathin_absolute));
                        DXCW_STRNCPY(write_entry.pathout_absolute, pathout_absolute.c_str(), sizeof(write_entry.pathout_absolute));
                        DXCW_STRNCPY(write_entry.target, str_target, sizeof(write_entry.target));
                        DXCW_STRNCPY(write_entry.entrypoint, str_entrypoint, sizeof(write_entry.entrypoint));
                    }

                    ++out_num_binaries;
                }
            }
        }

        json_t const* const jp_library = json_getProperty(j_entry, "library");
        if (jp_library)
        {
            if (json_getType(jp_library) != JSON_OBJ)
            {
                DXCW_LOG_WARN("entry #{} property \"library\" is not an object", num_entries);
                ++num_errors;
            }
            else
            {
                char const* const str_output = f_get_string_prop(jp_library, "output");
                if (!str_output)
                {
                    DXCW_LOG_WARN("skipping library on entry #{} which lacks required \"output\" text property", num_entries);
                    ++num_errors;
                    continue;
                }

                json_t const* const jp_exports = json_getProperty(jp_library, "exports");
                if (!jp_exports)
                {
                    DXCW_LOG_WARN(R"(skipping library of entry #{} which lacks required "exports" array property)", num_entries);
                    continue;
                }

                if (jp_exports->type != JSON_ARRAY)
                {
                    DXCW_LOG_WARN(R"("exports" property in library of entry #{} is not an array)", num_entries);
                    continue;
                }

                // determine the total amount of exports
                unsigned num_exports = 0;
                for (json_t const* j_exp = json_getChild(jp_exports); j_exp; j_exp = json_getSibling(j_exp))
                {
                    if (j_exp->type != JSON_TEXT && j_exp->type != JSON_OBJ)
                    {
                        DXCW_LOG_WARN(R"(an export element in library of entry #{} is neither string nor object)", num_entries);
                        continue;
                    }

                    ++num_exports;
                }

                if (num_exports == 0)
                {
                    DXCW_LOG_WARN("skipping library of entry #{} which specifies no exports", num_entries);
                    continue;
                }


                if (num_exports > (sizeof(shaderlist_library_entry_owning::exports_internal_names) / sizeof(char const*)))
                {
                    DXCW_LOG_WARN("too many exports specified in library of entry #{}", num_entries);
                    ++num_errors;
                    continue;
                }

                if (out_num_libraries < out_libraries.size())
                {
                    auto& write_entry = out_libraries[out_num_libraries];
                    auto const pathout_absolute = (base_path_fs / str_output).string();

                    DXCW_STRNCPY(write_entry.pathin, str_source, sizeof(write_entry.pathin));
                    DXCW_STRNCPY(write_entry.pathin_absolute, pathin_absolute.string().c_str(), sizeof(write_entry.pathin_absolute));
                    DXCW_STRNCPY(write_entry.pathout_absolute, pathout_absolute.c_str(), sizeof(write_entry.pathout_absolute));
                    write_entry.num_exports = uint8_t(num_exports);

                    unsigned exports_cursor = 0;
                    unsigned exports_strbuf_cursor = 0;

                    // extract entries

                    auto const f_add_export = [&](char const* internal_name, char const* exported_name) {
                        CC_ASSERT(internal_name && "fatal error");

                        char* const written_str_internal = write_entry.entrypoint_buffer + exports_strbuf_cursor;
                        std::strncpy(written_str_internal, internal_name, sizeof(write_entry.entrypoint_buffer) - exports_strbuf_cursor);

                        exports_strbuf_cursor += std::strlen(written_str_internal) + 1;
                        write_entry.exports_internal_names[exports_cursor] = written_str_internal;

                        if (exported_name)
                        {
                            char* const written_str_exported = write_entry.entrypoint_buffer + exports_strbuf_cursor;
                            std::strncpy(written_str_exported, internal_name, sizeof(write_entry.entrypoint_buffer) - exports_strbuf_cursor);

                            exports_strbuf_cursor += std::strlen(written_str_exported) + 1;
                            write_entry.exports_exported_names[exports_cursor] = written_str_exported;
                        }
                        else
                        {
                            write_entry.exports_exported_names[exports_cursor] = nullptr;
                        }

                        ++exports_cursor;
                    };


                    for (json_t const* j_exp = json_getChild(jp_exports); j_exp; j_exp = json_getSibling(j_exp))
                    {
                        if (j_exp->type != JSON_TEXT && j_exp->type != JSON_OBJ)
                        {
                            continue;
                        }

                        if (j_exp->type == JSON_TEXT)
                        {
                            f_add_export(j_exp->u.value, nullptr);
                        }
                        else if (j_exp->type == JSON_OBJ)
                        {
                            char const* str_internal = f_get_string_prop(j_exp, "internal");
                            char const* str_export = f_get_string_prop(j_exp, "export");

                            if (!str_internal)
                            {
                                DXCW_LOG_WARN(R"(an export element in library of entry #{} does not specify the required "internal" property - name of the export in HLSL)",
                                              num_entries);
                                continue;
                            }

                            f_add_export(str_internal, str_export);
                        }
                    }
                }

                ++out_num_libraries;
            }
        }
    }

    return true;

l_parse_error:
    DXCW_LOG_ERROR("parse error in json shader list {}", shaderlist_file);
    return false;
}

unsigned dxcw::parse_includes(const char* source_path, const char* include_path, dxcw::include_entry* out_include_entries, unsigned max_num_out)
{
    CC_ASSERT(out_include_entries != nullptr && max_num_out > 0 && "Output must not be empty");

    std::error_code ec;
    auto const include_path_fs = std::filesystem::canonical(include_path, ec);
    if (ec)
    {
        return 0;
    }

    unsigned num_includes = 0;

    std::string line;
    std::string token1;
    std::string token2;

    if (out_include_entries == nullptr)
        max_num_out = 0;

    auto const f_add_file = [&](char const* path, unsigned num_prev_includes) -> unsigned {
        std::fstream in_file(path);
        if (!in_file.good())
            return 0;

        unsigned num_added_includes = 0;

        while (std::getline(in_file, line))
        {
            if (line.empty() || line[0] != '#')
                continue;

            std::stringstream ss(line);
            if (ss >> token1 && ss >> token2 && std::strcmp(token1.c_str(), "#include") == 0 && token2.length() > 2)
            {
                // this line is formatted like an #include directive

                std::string absolute_include;
                {
                    std::string const include_without_quotes = token2.substr(1, token2.length() - 2);
                    // look the include up by completing it from root ("include_path")
                    ec.clear();
                    auto const include_path_from_root_fs = std::filesystem::canonical(include_path_fs / include_without_quotes, ec);

                    if (ec)
                    {
                        // look it up by completing it from the current folder instead
                        ec.clear();
                        auto const include_path_from_local_fs
                            = std::filesystem::canonical(std::filesystem::path(path).remove_filename() / include_without_quotes, ec);

                        if (ec)
                        {
                            // include is invalid, silently fail (DXC will warn about this already)
                            // printf("        - parsed include %s which cannot be found\n", include_without_quotes.c_str());
                            continue;
                        }
                        else
                        {
                            absolute_include = include_path_from_local_fs.string();
                        }
                    }
                    else
                    {
                        absolute_include = include_path_from_root_fs.string();
                    }
                }

                // check if already existing
                bool preexists = false;
                for (auto i = 0u; i < num_prev_includes + num_added_includes; ++i)
                {
                    if (std::strcmp(out_include_entries[i].includepath_absolute, absolute_include.c_str()) == 0)
                    {
                        // printf("        - found include %s which already exists\n", absolute_include.c_str());
                        preexists = true;
                        break;
                    }
                }

                if (!preexists)
                {
                    auto const next_index = num_prev_includes + num_added_includes;
                    if (next_index >= max_num_out)
                    {
                        return num_added_includes;
                    }

                    DXCW_STRNCPY(out_include_entries[next_index].includepath_absolute, absolute_include.c_str(),
                                 sizeof(out_include_entries[0].includepath_absolute));
                    ++num_added_includes;
                }
            }
        }

        // printf("        added file %s, got %d new includes\n", path, num_added_includes);
        return num_added_includes;
    };

    char const* next_file_to_add = source_path;
    int include_cursor = -1;
    unsigned num_latest_addition = 0;

    do
    {
        if (include_cursor >= 0)
            next_file_to_add = out_include_entries[include_cursor].includepath_absolute;

        //        printf("    cursor %d, num_includes: %u, next file: %s\n", include_cursor, num_includes, next_file_to_add);

        num_latest_addition = f_add_file(next_file_to_add, num_includes);

        num_includes += num_latest_addition;
        ++include_cursor;

        //        printf("    latest addition: %d (cursor %d vs num %d)\n", num_latest_addition, include_cursor, num_includes);

    } while (unsigned(include_cursor) < num_includes);

    return num_includes;
}
