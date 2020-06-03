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

bool parse_target(char const* str, phi::sc::target& out_tgt)
{
    if (std::strlen(str) < 1)
    {
        return false;
    }

    // first char is enough
    switch (str[0])
    {
    case 'v':
        out_tgt = phi::sc::target::vertex;
        return true;
    case 'h':
        out_tgt = phi::sc::target::hull;
        return true;
    case 'd':
        out_tgt = phi::sc::target::domain;
        return true;
    case 'g':
        out_tgt = phi::sc::target::geometry;
        return true;
    case 'p':
        out_tgt = phi::sc::target::pixel;
        return true;
    case 'c':
        out_tgt = phi::sc::target::compute;
        return true;
    default:
        return false;
    }
}
}

bool phi::sc::write_binary_to_file(const phi::sc::binary& binary, const char* path, const char* ending)
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

bool phi::sc::compile_shader(phi::sc::compiler& compiler, const char* source_path, const char* shader_target, const char* entrypoint, const char* output_path)
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


        phi::sc::target parsed_target;
        if (!parse_target(shader_target, parsed_target))
        {
            return false;
        }
        else
        {
            auto const filename_fs = std::filesystem::path(source_path).filename();

            // path.c_str() gives char const* on non-win32, but we're lucky to have this param be optional

            auto const f_path_to_wchar = [&](auto const& path) -> wchar_t const* {
                // this must be a templated lambda so the first body doesn't compile if the condition is false
                if constexpr (std::is_same_v<typename std::decay_t<decltype(path)>::value_type, wchar_t>) // the condition must directly depend on the template argument
                {
                    return path.c_str();
                }
                else
                {
                    return nullptr;
                }
            };

            auto dxil_binary = compiler.compile_binary(content.c_str(), entrypoint, parsed_target, phi::sc::output::dxil, f_path_to_wchar(filename_fs));
            phi::sc::write_binary_to_file(dxil_binary, output_path, "dxil");

            if (dxil_binary.internal_blob != nullptr)
            {
                // only destroy and re-run if the first one worked
                phi::sc::destroy_blob(dxil_binary.internal_blob);

                auto spv_binary = compiler.compile_binary(content.c_str(), entrypoint, parsed_target, phi::sc::output::spirv, f_path_to_wchar(filename_fs));
                phi::sc::write_binary_to_file(spv_binary, output_path, "spv");
                phi::sc::destroy_blob(spv_binary.internal_blob);
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

int phi::sc::compile_shaderlist(phi::sc::compiler& compiler, const char* shaderlist_file, int* out_num_errors)
{
    std::fstream in_file(shaderlist_file);
    if (!in_file.good())
    {
        std::fprintf(stderr, "ERROR: failed to open shaderlist file at %s\n", shaderlist_file);
        return -1;
    }

    // set the working directory to the folder containing the list this was invoked with
    auto const prev_workdir = std::filesystem::current_path();
    auto const base_path_fs = std::filesystem::absolute(std::filesystem::path(shaderlist_file).remove_filename());
    std::filesystem::current_path(base_path_fs);

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
            ++num_shaders;
            //                    printf("Invoked with %s %s %s %s\n", pathin.c_str(), target.c_str(), entrypoint.c_str(), pathout.c_str());
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

    // restore workdir
    std::filesystem::current_path(prev_workdir);

    if (out_num_errors)
        *out_num_errors = num_errors;

    return num_shaders;
}
