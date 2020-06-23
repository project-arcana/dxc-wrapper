#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <dxc-wrapper/compiler.hh>
#include <dxc-wrapper/file_util.hh>

#include "common/file_watch.hh"

namespace
{
void print_error()
{
    std::printf("usage: ./dxcw [input file] [entrypoint] [target] [output file without ending]\n"
                "  target is one of: vs, ds, hs, gs, ps, cs\n\n"
                "or: ./dxcw [list file]\n"
                "  list file contains normal arguments line-by-line\n"
                "or: ./dxcw -w [list file]\n"
                "  watch mode - list file contains normal arguments line-by-line\n");
}
}

int main(int argc, char const* argv[])
{
    if (argc == 2)
    {
        // shaderlist mode
        // ./dxcw path/to/shaderlist.txt

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
    else if (argc == 3)
    {
        // shaderlist watch mode
        // ./dxcw [-w|--watch] path/to/shaderlist.txt

        if (std::strcmp(argv[1], "-w") != 0 && std::strcmp(argv[1], "--watch") != 0)
        {
            print_error();
            return 1;
        }

        char const* const arg_pathin = argv[2];

        if (!std::fstream(arg_pathin).good())
        {
            std::fprintf(stderr, "ERROR: failed to open shaderlist file at %s\n", arg_pathin);
            return 1;
        }

        dxcw::compiler compiler;
        compiler.initialize();

        unsigned num_shaders = 0;
        dxcw::FileWatch::SharedFlag shaderlist_watch = dxcw::FileWatch::watchFile(arg_pathin);

        // set the working directory to the folder containing the list this was invoked with
        auto const base_path_fs = std::filesystem::absolute(std::filesystem::path(arg_pathin).remove_filename()).string();

        struct auxilliary_watch_entry
        {
            dxcw::FileWatch::SharedFlag flags[10];
            dxcw::include_entry include_entries[10];
            unsigned num_files = 0;
        };

        std::vector<dxcw::shaderlist_entry_owning> watch_entries;
        std::vector<dxcw::FileWatch::SharedFlag> watch_flags;

        std::vector<auxilliary_watch_entry> watch_aux;

        auto const f_compile_entry = [&](dxcw::shaderlist_entry_owning const& entry) -> bool {
            auto const success
                = dxcw::compile_shader(compiler, entry.pathin_absolute, entry.target, entry.entrypoint, entry.pathout_absolute, base_path_fs.c_str());

            if (success)
                printf("compiled %s (%s; %s)\n", entry.pathin, entry.target, entry.entrypoint);
            else
                fprintf(stderr, "error compiling %s (%s; %s)\n", entry.pathin, entry.target, entry.entrypoint);

            return success;
        };

        auto const f_refresh_entries = [&]() -> bool {
            num_shaders = dxcw::parse_shaderlist(arg_pathin, nullptr, 0);
            if (num_shaders == unsigned(-1))
                return false;

            watch_entries.resize(num_shaders);
            watch_flags.resize(num_shaders);

            dxcw::parse_shaderlist(arg_pathin, watch_entries.data(), watch_entries.size());
            for (auto i = 0u; i < num_shaders; ++i)
            {
                // create file watch
                watch_flags[i] = dxcw::FileWatch::watchFile(watch_entries[i].pathin_absolute);
                watch_flags[i]->clear();

                //
                f_compile_entry(watch_entries[i]);
            }

            return true;
        };

        f_refresh_entries();

        printf("\nwatching shaderlist file at %s\n", base_path_fs.c_str());
        // clear initial watch
        shaderlist_watch->clear();

        while (true)
        {
            // sleep
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(250ms);

            if (shaderlist_watch->isChanged())
            {
                printf("shaderlist file changed, recompiling all shaders\n");

                // changes in shaderlist - refresh
                if (!f_refresh_entries())
                    return 1;

                shaderlist_watch->clear();

                continue;
            }

            for (auto i = 0u; i < num_shaders; ++i)
            {
                if (watch_flags[i]->isChanged())
                {
                    // recompile just this file
                    f_compile_entry(watch_entries[i]);
                    watch_flags[i]->clear();
                }
            }
        }
    }
    else if (argc == 5)
    {
        // normal shader mode
        // ./dxcw path/to/in.hlsl main_vs vs path/to/out

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
