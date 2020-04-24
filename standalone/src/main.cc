#include <cstdio>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

#include <clean-core/assert.hh>

#include <dxc-wrapper/compiler.hh>
#include <dxc-wrapper/file_util.hh>

void print_error()
{
    std::printf("usage: dxc-standalone [input file] [entrypoint] [target] [output file without ending]\n"
                "  target is one of: vs, ds, hs, gs, ps, cs\n\n"
                "or: dxc-standalone [list file]\n"
                "  list file contains normal arguments line-by-line\n");
}


int main(int argc, char const* argv[])
{
    if (argc == 2)
    {
        char const* const arg_pathin = argv[1];
        std::fstream in_file(arg_pathin);
        if (!in_file.good())
        {
            std::fprintf(stderr, "ERROR: failed to open input file at %s\n", arg_pathin);
            return 1;
        }
        else
        {
            // set the working directory to the folder containing the list this was invoked with
            auto base_path_fs = std::filesystem::absolute(std::filesystem::path(arg_pathin).remove_filename());
            std::filesystem::current_path(base_path_fs);

            phi::sc::compiler compiler;
            compiler.initialize();
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
                    std::fprintf(stderr, "ERROR: failed to parse %s:%d:\n  %s\n\n", arg_pathin, num_lines, line.c_str());
                    ++num_errors;
                }
            }

            compiler.destroy();

            std::printf("compiled %d shaders, %d errors\n", num_shaders, num_errors);
            return (num_errors == 0) ? 0 : 1;
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
        auto const success = phi::sc::compile_shader(compiler, arg_pathin, arg_target, arg_entrypoint, arg_pathout);

        if (!success)
            print_error();

        compiler.destroy();

        return success ? 0 : 1;
    }
    else
    {
        print_error();
        return 1;
    }
}
