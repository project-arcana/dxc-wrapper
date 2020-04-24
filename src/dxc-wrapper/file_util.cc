#include "file_util.hh"

#include <clean-core/assert.hh>

#include <cstdio>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

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

void phi::sc::write_binary_to_file(const phi::sc::binary& binary, const char* path, const char* ending)
{
    if (binary.data != nullptr)
    {
        char outpath[1024];
        std::snprintf(outpath, sizeof(outpath), "%s.%s", path, ending);

        //        printf("writing %ls\n", std::filesystem::absolute(std::filesystem::path(outpath)).c_str());

        // recursively create directories required for the output
        std::filesystem::create_directories(std::filesystem::path(outpath).remove_filename());

        auto outfile = std::fstream(outpath, std::ios::out | std::ios::binary);
        CC_RUNTIME_ASSERT(outfile.good() && "failed to write shader");
        outfile.write(reinterpret_cast<char const*>(binary.data), std::streamsize(binary.size));
        outfile.close();
    }
}

bool phi::sc::compile_shader(phi::sc::compiler& compiler, const char* arg_pathin, const char* arg_target, const char* arg_entrypoint, const char* arg_pathout)
{
    std::fstream in_file(arg_pathin);
    if (!in_file.good())
    {
        std::printf("Failed to open input file at %s\n", arg_pathin);
        return false;
    }
    else
    {
        auto const content = readall(in_file);


        phi::sc::target shader_target;
        if (!parse_target(arg_target, shader_target))
        {
            return false;
        }
        else
        {
            auto const filename_fs = std::filesystem::path(arg_pathin).filename();

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

            auto dxil_binary = compiler.compile_binary(content.c_str(), arg_entrypoint, shader_target, phi::sc::output::dxil, f_path_to_wchar(filename_fs));
            phi::sc::write_binary_to_file(dxil_binary, arg_pathout, "dxil");

            if (dxil_binary.internal_blob != nullptr)
            {
                // only destroy and re-run if the first one worked
                phi::sc::destroy_blob(dxil_binary.internal_blob);

                auto spv_binary = compiler.compile_binary(content.c_str(), arg_entrypoint, shader_target, phi::sc::output::spirv, f_path_to_wchar(filename_fs));
                phi::sc::write_binary_to_file(spv_binary, arg_pathout, "spv");
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
