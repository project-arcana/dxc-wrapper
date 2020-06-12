#include <cstdio>

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

        dxcw::compiler compiler;
        compiler.initialize();

        dxcw::shaderlist_compilation_result res;
        dxcw::compile_shaderlist(compiler, arg_pathin, &res);

        compiler.destroy();

        if (res.num_shaders_detected == -1)
        {
            print_error();
            return 1;
        }
        else
        {
            std::printf("compiled %d shaders, %d errors\n", res.num_shaders_detected, res.num_errors);
            return (res.num_errors == 0) ? 0 : 1;
        }
    }
    else if (argc == 5)
    {
        char const* const arg_pathin = argv[1];
        char const* const arg_entrypoint = argv[2];
        char const* const arg_target = argv[3];
        char const* const arg_pathout = argv[4];

        dxcw::compiler compiler;
        compiler.initialize();
        auto const success = dxcw::compile_shader(compiler, arg_pathin, arg_target, arg_entrypoint, arg_pathout);

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
