#include <cstdio>
#include <cstring>

#include <fstream>
#include <sstream>

#include <clean-core/assert.hh>

#include <dxc-wrapper/compiler.hh>

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

void print_error()
{
    printf("usage: dxc-standalone [input file] [entrypoint] [target] [output file without ending]\n");
    printf("  target is one of: vs, ds, hs, gs, ps, cs\n\n");
    printf("or: dxc-standalone [list file]\n");
    printf("  list file contains normal arguments line-by-line\n");
}

std::string readall(std::istream& in)
{
    std::string ret;
    char buffer[4096];
    while (in.read(buffer, sizeof(buffer)))
        ret.append(buffer, sizeof(buffer));
    ret.append(buffer, in.gcount());
    return ret;
}

void write_binary_to_file(phi::sc::binary const& binary, char const* path, char const* ending)
{
    if (binary.data != nullptr)
    {
        char outpath[1024];
        std::snprintf(outpath, sizeof(outpath), "%s.%s", path, ending);

        auto outfile = std::fstream(outpath, std::ios::out | std::ios::binary);
        CC_RUNTIME_ASSERT(outfile.good() && "failed to write shader");
        outfile.write(reinterpret_cast<char const*>(binary.data), binary.size);
        outfile.close();
    }
}


void compile_shader(phi::sc::compiler& compiler, char const* arg_pathin, char const* arg_target, char const* arg_entrypoint, char const* arg_pathout)
{
    std::fstream in_file(arg_pathin);
    if (!in_file.good())
    {
        printf("Failed to open input file at %s\n", arg_pathin);
    }
    else
    {
        auto const content = readall(in_file);

        phi::sc::target shader_target;
        if (!parse_target(arg_target, shader_target))
        {
            print_error();
        }
        else
        {
            auto dxil_binary = compiler.compile_binary(content.c_str(), arg_entrypoint, shader_target, phi::sc::output::dxil);
            write_binary_to_file(dxil_binary, arg_pathout, "dxil");

            if (dxil_binary.internal_blob != nullptr)
            {
                // only destroy and re-run if the first one worked
                phi::sc::destroy_blob(dxil_binary.internal_blob);

                auto spv_binary = compiler.compile_binary(content.c_str(), arg_entrypoint, shader_target, phi::sc::output::spirv);
                write_binary_to_file(spv_binary, arg_pathout, "spv");
                phi::sc::destroy_blob(spv_binary.internal_blob);
            }
        }
    }
}

int main(int argc, char const* argv[])
{
    if (argc == 2)
    {
        char const* const arg_pathin = argv[1];

        std::fstream in_file(arg_pathin);
        if (!in_file.good())
        {
            printf("Failed to open input file at %s\n", arg_pathin);
        }
        else
        {
            phi::sc::compiler compiler;
            compiler.initialize();
            std::string line;

            std::string pathin;
            std::string target;
            std::string entrypoint;
            std::string pathout;

            while (std::getline(in_file, line))
            {
                std::stringstream ss(line);

                if (ss >> pathin && ss >> entrypoint && ss >> target && ss >> pathout)
                {
//                    printf("Invoked with %s %s %s %s\n", pathin.c_str(), target.c_str(), entrypoint.c_str(), pathout.c_str());
                    compile_shader(compiler, pathin.c_str(), target.c_str(), entrypoint.c_str(), pathout.c_str());
                }
            }

            compiler.destroy();
        }
    }
    else if (argc == 5)
    {
        char const* const arg_pathin = argv[1];
        char const* const arg_entrypoint = argv[2];
        char const* const arg_target = argv[3];
        char const* const arg_pathout = argv[4];

        phi::sc::compiler compiler;
        compiler.initialize();
        compile_shader(compiler, arg_pathin, arg_target, arg_entrypoint, arg_pathout);
        compiler.destroy();
    }
    else
    {
        print_error();
    }
}
