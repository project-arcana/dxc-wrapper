#include "file_util.hh"

#include <cstdio>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

#include <clean-core/assert.hh>

#include <dxc-wrapper/compiler.hh>

namespace
{
std::string readall(std::istream& in)
{
    std::string ret;
    char buffer[4096];
    while (in.read(buffer, sizeof(buffer)))
        ret.append(buffer, sizeof(buffer));
    ret.append(buffer, size_t(in.gcount()));
    return ret;
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
        std::fprintf(stderr, "Failed to write shader to %s.%s\n", path, ending);
        return false;
    }

    outfile.write(reinterpret_cast<char const*>(binary.data), std::streamsize(binary.size));
    outfile.close();
    return true;
}

bool dxcw::compile_shader(dxcw::compiler& compiler, const char* source_path, const char* shader_target, const char* entrypoint, const char* output_path, const char* optional_include_dir)
{
    std::fstream in_file(source_path);
    if (!in_file.good())
    {
        std::fprintf(stderr, "Failed to open input file at %s\n", source_path);
        return false;
    }
    else
    {
        auto const content = readall(in_file);


        dxcw::target parsed_target;
        if (!parse_target(shader_target, parsed_target))
        {
            return false;
        }
        else
        {
#ifdef CC_OS_WINDOWS
            auto dxil_binary = compiler.compile_binary(content.c_str(), entrypoint, parsed_target, dxcw::output::dxil, optional_include_dir, false, source_path);

            if (dxil_binary.internal_blob == nullptr)
                return false;

            dxcw::write_binary_to_file(dxil_binary, output_path, "dxil");
            dxcw::destroy_blob(dxil_binary.internal_blob);
#endif
            // On non-windows, DXIL can be compiled but not signed which makes it mostly useless
            // requiring DXIL on linux would be a pretty strange path but can be supported with more tricks

            auto spv_binary = compiler.compile_binary(content.c_str(), entrypoint, parsed_target, dxcw::output::spirv, optional_include_dir, false, source_path);
            if (spv_binary.internal_blob == nullptr)
                return false;

            dxcw::write_binary_to_file(spv_binary, output_path, "spv");
            dxcw::destroy_blob(spv_binary.internal_blob);
            return true;
        }
    }
}

void dxcw::compile_shaderlist(dxcw::compiler& compiler, const char* shaderlist_file, shaderlist_compilation_result* out_results)
{
    auto const f_onerror = [&]() -> void {
        std::fprintf(stderr, "ERROR: failed to open shaderlist file at %s\n", shaderlist_file);
        if (out_results)
            *out_results = {-1, 1};
    };

    std::fstream in_file(shaderlist_file);
    if (!in_file.good())
    {
        f_onerror();
        return;
    }

    // set the working directory to the folder containing the list this was invoked with
    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_file).remove_filename(), ec);
    if (ec)
    {
        f_onerror();
        return;
    }

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
                std::fprintf(stderr, "ERROR: shader %s not found (line %d)\n", pathin.c_str(), num_lines);
                ++num_errors;
                continue;
            }

            ++num_shaders;
            auto const success = compile_shader(compiler, pathin.c_str(), target.c_str(), entrypoint.c_str(), pathout.c_str());

            if (!success)
                ++num_errors;
        }
        else
        {
            std::fprintf(stderr, "ERROR: failed to parse %s:%d:\n  %s\n\n", shaderlist_file, num_lines, line.c_str());
            ++num_errors;
        }
    }

    if (out_results)
        *out_results = {num_shaders, num_errors};
}

unsigned dxcw::parse_shaderlist(const char* shaderlist_file, dxcw::shaderlist_entry_owning* out_entries, unsigned max_num_out)
{
    std::fstream in_file(shaderlist_file);
    if (!in_file.good())
    {
        std::fprintf(stderr, "ERROR: failed to open shaderlist file at %s\n", shaderlist_file);
        return unsigned(-1);
    }

    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_file).remove_filename(), ec);
    if (ec)
    {
        std::fprintf(stderr, "ERROR: failed to open shaderlist file at %s\n", shaderlist_file);
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
                std::fprintf(stderr, "ERROR: shader %s not found (line %d)\n", pathin.c_str(), num_lines);
                ++num_errors;
                continue;
            }

            if (num_shaders < max_num_out)
            {
                auto& write_entry = out_entries[num_shaders];
                auto const pathout_absolute = (base_path_fs / pathout).string();
                std::strncpy(write_entry.pathin, pathin.c_str(), sizeof(write_entry.pathin));
                std::strncpy(write_entry.pathin_absolute, pathin_absolute_fs.string().c_str(), sizeof(write_entry.pathin_absolute));
                std::strncpy(write_entry.pathout_absolute, pathout_absolute.c_str(), sizeof(write_entry.pathout_absolute));
                std::strncpy(write_entry.target, target.c_str(), sizeof(write_entry.target));
                std::strncpy(write_entry.entrypoint, entrypoint.c_str(), sizeof(write_entry.entrypoint));
            }

            ++num_shaders;
        }
        else
        {
            std::fprintf(stderr, "ERROR: failed to parse %s:%d:\n  %s\n\n", shaderlist_file, num_lines, line.c_str());
            ++num_errors;
        }
    }

    return num_shaders;
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
                ec.clear();
                auto const path_absolute_fs = std::filesystem::canonical(include_path_fs / token2.substr(1, token2.length() - 2), ec);
                if (ec)
                {
                    // include is invalid, silently fail (DXC will warn about this already)
                    continue;
                }
                auto const path_absolute = path_absolute_fs.string();

                // check if already existing
                bool preexists = false;
                for (auto i = 0u; i < num_prev_includes + num_added_includes; ++i)
                {
                    if (std::strcmp(out_include_entries[i].includepath_absolute, path_absolute.c_str()) == 0)
                    {
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

                    std::strncpy(out_include_entries[next_index].includepath_absolute, path_absolute.c_str(), sizeof(out_include_entries[0].includepath_absolute));
                    ++num_added_includes;
                }
            }
        }

        return num_added_includes;
    };

    char const* next_file_to_add = source_path;
    int include_cursor = -1;
    unsigned num_latest_addition = 0;

    do
    {
        if (include_cursor >= 0)
            next_file_to_add = out_include_entries[include_cursor].includepath_absolute;

        // printf("    cursor %d, num_includes: %u, next file: %s\n", include_cursor, num_includes, next_file_to_add);

        num_latest_addition = f_add_file(next_file_to_add, num_includes);
        num_includes += num_latest_addition;
        ++include_cursor;
    } while (unsigned(include_cursor) < num_includes);

    return num_includes;
}
