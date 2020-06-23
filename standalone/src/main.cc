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

        dxcw::compiler compiler;
        compiler.initialize();

        unsigned num_shaders = 0;
        dxcw::FileWatch::SharedFlag shaderlist_watch = dxcw::FileWatch::watchFile(arg_pathin);
        if (!shaderlist_watch)
        {
            std::fprintf(stderr, "ERROR: failed to open shaderlist file at %s\n", arg_pathin);
            return 1;
        }

        constexpr unsigned sc_max_num_includes = 10;
        struct auxilliary_watch_entry
        {
            dxcw::FileWatch::SharedFlag main_flag;
            dxcw::FileWatch::SharedFlag include_flags[sc_max_num_includes];
            dxcw::include_entry include_entries[sc_max_num_includes];
            unsigned num_files = 0;
        };

        std::vector<dxcw::shaderlist_entry_owning> watch_entries;
        std::vector<auxilliary_watch_entry> watch_aux;

        std::error_code ec;
        auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(arg_pathin).remove_filename(), ec).string();

        if (ec)
        {
            std::fprintf(stderr, "ERROR: failed to open shaderlist file at %s\n", arg_pathin);
            return 1;
        }

        auto const f_compile_entry = [&](dxcw::shaderlist_entry_owning const& entry) -> bool {
            auto const success
                = dxcw::compile_shader(compiler, entry.pathin_absolute, entry.target, entry.entrypoint, entry.pathout_absolute, base_path_fs.c_str());

            if (success)
                printf("compiled %s (%s; %s)\n", entry.pathin, entry.target, entry.entrypoint);
            else
                fprintf(stderr, "error compiling %s (%s; %s)\n", entry.pathin, entry.target, entry.entrypoint);

            return success;
        };

        auto const f_refresh_includes = [&](auxilliary_watch_entry& aux_entry, char const* shader_path) -> void {
            aux_entry.num_files = dxcw::parse_includes(shader_path, base_path_fs.c_str(), aux_entry.include_entries, sc_max_num_includes);

            for (auto j = 0u; j < aux_entry.num_files; ++j)
            {
                aux_entry.include_flags[j] = dxcw::FileWatch::watchFile(aux_entry.include_entries[j].includepath_absolute);
            }
            for (auto j = aux_entry.num_files; j < sc_max_num_includes; ++j)
            {
                aux_entry.include_flags[j] = nullptr;
            }
        };

        auto const f_refresh_all_entries = [&]() -> bool {
            num_shaders = dxcw::parse_shaderlist(arg_pathin, nullptr, 0);
            if (num_shaders == unsigned(-1))
                return false;

            watch_entries.resize(num_shaders);
            watch_aux.resize(num_shaders);

            num_shaders = dxcw::parse_shaderlist(arg_pathin, watch_entries.data(), unsigned(watch_entries.size()));
            for (auto i = 0u; i < num_shaders; ++i)
            {
                // recreate main watch flag
                watch_aux[i].main_flag = dxcw::FileWatch::watchFile(watch_entries[i].pathin_absolute);
                // refresh include flags
                f_refresh_includes(watch_aux[i], watch_entries[i].pathin_absolute);
                // compile
                f_compile_entry(watch_entries[i]);
            }

            return true;
        };

        f_refresh_all_entries();

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
                if (!f_refresh_all_entries())
                    return 1;

                shaderlist_watch->clear();

                continue;
            }

            for (auto i = 0u; i < num_shaders; ++i)
            {
                auto& entry = watch_aux[i];

                if (entry.main_flag->isChanged())
                {
                    // main file changed, refresh includes and recompile
                    f_refresh_includes(watch_aux[i], watch_entries[i].pathin_absolute);
                    f_compile_entry(watch_entries[i]);
                    entry.main_flag->clear();
                }
                else
                {
                    for (auto j = 0u; j < entry.num_files; ++j)
                    {
                        if (entry.include_flags[j]->isChanged())
                        {
                            // single include changed, refresh includes and recompile
                            f_refresh_includes(entry, watch_entries[i].pathin_absolute);
                            f_compile_entry(watch_entries[i]);
                            break;
                        }
                    }
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
