#include "entry.hh"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <clean-core/alloc_array.hh>

#include <nexus/args.hh>

#include <dxc-wrapper/common/log.hh>
#include <dxc-wrapper/compiler.hh>
#include <dxc-wrapper/file_util.hh>

#include "common/file_watch.hh"

namespace
{
volatile int gv_keep_running = 1;
void interrupt_handler(int) { gv_keep_running = 0; }
}

int dxcw::compile_shader_single(const nx::args& args)
{
    auto const pos_args = args.positional_args();
    if (pos_args.size() < 4)
    {
        DXCW_LOG_ERROR("fatal error, run ./dxcw -h for usage");
        return 1;
    }

    dxcw::compiler compiler;
    compiler.initialize();
    auto const success = dxcw::compile_shader(compiler, pos_args[0].c_str(), pos_args[1].c_str(), pos_args[2].c_str(), pos_args[3].c_str());

    if (!success)
    {
        DXCW_LOG_ERROR("failed to read or compile shader, no output written");
    }

    compiler.destroy();

    return success ? 0 : 1;
}


int dxcw::compile_shaderlist_single(const char* shaderlist_path)
{
    dxcw::compiler compiler;
    compiler.initialize();

    dxcw::shaderlist_compilation_result res;
    bool const success = dxcw::compile_shaderlist(compiler, shaderlist_path, &res);

    compiler.destroy();

    if (!success)
    {
        return 1;
    }
    else
    {
        DXCW_LOG("compiled {} shaders, {} errors", res.num_shaders_detected, res.num_errors);
        return (res.num_errors == 0) ? 0 : 1;
    }
}

int dxcw::compile_shaderlist_watch(const char* shaderlist_path)
{
    dxcw::compiler compiler;
    compiler.initialize();

    unsigned num_shaders = 0;
    dxcw::FileWatch::SharedFlag shaderlist_watch = dxcw::FileWatch::watchFile(shaderlist_path);
    if (!shaderlist_watch)
    {
        DXCW_LOG_ERROR("failed to open shaderlist file at {}", shaderlist_path);
        return 1;
    }

    constexpr unsigned sc_max_num_includes = 10;
    struct auxilliary_watch_entry
    {
        dxcw::FileWatch::SharedFlag main_flag;
        dxcw::FileWatch::SharedFlag include_flags[sc_max_num_includes];
        dxcw::include_entry include_entries[sc_max_num_includes];
        unsigned num_files = 0;
        bool was_last_compilation_successful = true;
    };

    std::vector<dxcw::shaderlist_binary_entry_owning> watch_entries;
    std::vector<auxilliary_watch_entry> watch_aux;

    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_path).remove_filename(), ec).string();

    if (ec)
    {
        DXCW_LOG_ERROR("failed to open shaderlist file at {}", shaderlist_path);
        return 1;
    }

    auto const f_compile_entry = [&](dxcw::shaderlist_binary_entry_owning const& entry) -> bool {
        auto const success
            = dxcw::compile_shader(compiler, entry.pathin_absolute, entry.target, entry.entrypoint, entry.pathout_absolute, base_path_fs.c_str());

        if (success)
            DXCW_LOG("compiled {} ({}; {})", entry.pathin, entry.target, entry.entrypoint);
        else
            DXCW_LOG_WARN("error compiling {} ({}; {})", entry.pathin, entry.target, entry.entrypoint);

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
        num_shaders = dxcw::parse_shaderlist(shaderlist_path, nullptr, 0);
        if (num_shaders == unsigned(-1))
            return false;

        watch_entries.resize(num_shaders);
        watch_aux.resize(num_shaders);

        num_shaders = dxcw::parse_shaderlist(shaderlist_path, watch_entries.data(), unsigned(watch_entries.size()));
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

    bool const initial_success = f_refresh_all_entries();

    if (!initial_success)
    {
        // shouldn't happen because of the canonical path
        DXCW_LOG_ERROR("failed to open shaderlist file at {}", base_path_fs.c_str());
        return 1;
    }

    DXCW_LOG("watching shaderlist file at {}", base_path_fs.c_str());
    // clear initial watch
    shaderlist_watch->clear();

    std::signal(SIGINT, interrupt_handler);
    while (gv_keep_running)
    {
        // sleep
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(250ms);

        if (shaderlist_watch->isChanged())
        {
            DXCW_LOG("shaderlist file changed, recompiling all shaders");

            // changes in shaderlist - refresh
            if (!f_refresh_all_entries())
            {
                DXCW_LOG_ERROR("shaderlist file not readable after changes, aborting");
                return 1;
            }

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

    compiler.destroy();
    DXCW_LOG("stopped watching");
    return 0;
}

int dxcw::compile_shaderlist_json_single(const char* shaderlist_json)
{
    dxcw::compiler compiler;
    compiler.initialize();
    dxcw::shaderlist_compilation_result res;
    bool const success = compile_shaderlist_json(compiler, shaderlist_json, &res);
    compiler.destroy();

    if (!success)
    {
        return 1;
    }
    else
    {
        DXCW_LOG("compiled {} shaders, {} libraries, {} errors", res.num_shaders_detected, res.num_libraries_detected, res.num_errors);
        return (res.num_errors == 0) ? 0 : 1;
    }
}

int dxcw::compile_shaderlist_json_watch(const char* shaderlist_json, cc::allocator* scratch_alloc)
{
    dxcw::compiler compiler;
    compiler.initialize();

    unsigned num_shaders = 0;
    unsigned num_libraries = 0;
    dxcw::FileWatch::SharedFlag shaderlist_watch = dxcw::FileWatch::watchFile(shaderlist_json);
    if (!shaderlist_watch)
    {
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", shaderlist_json);
        return 1;
    }

    constexpr unsigned sc_max_num_includes = 10;
    struct auxilliary_watch_entry
    {
        dxcw::FileWatch::SharedFlag main_flag;
        dxcw::FileWatch::SharedFlag include_flags[sc_max_num_includes];
        dxcw::include_entry include_entries[sc_max_num_includes];
        unsigned num_files = 0;
        bool was_last_compilation_successful = true;
    };

    std::vector<dxcw::shaderlist_binary_entry_owning> watch_binary_entries;
    std::vector<dxcw::shaderlist_library_entry_owning> watch_library_entries;
    std::vector<auxilliary_watch_entry> watch_binary_aux;
    std::vector<auxilliary_watch_entry> watch_library_aux;

    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_json).remove_filename(), ec).string();

    if (ec)
    {
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", shaderlist_json);
        return 1;
    }

    auto const f_compile_entry = [&](dxcw::shaderlist_binary_entry_owning const& entry) -> bool {
        auto const success = dxcw::compile_shader(compiler, entry.pathin_absolute, entry.target, entry.entrypoint, entry.pathout_absolute,
                                                  base_path_fs.c_str(), scratch_alloc);

        if (success)
            DXCW_LOG("compiled {} ({}; {})", entry.pathin, entry.target, entry.entrypoint);
        else
            DXCW_LOG_WARN("error compiling {} ({}; {})", entry.pathin, entry.target, entry.entrypoint);

        return success;
    };

    auto const f_compile_library = [&](dxcw::shaderlist_library_entry_owning const& entry) -> bool {
        auto exports = cc::alloc_array<dxcw::library_export>::uninitialized(entry.num_exports, scratch_alloc);

        for (auto i = 0u; i < entry.num_exports; ++i)
        {
            exports[i].type = static_cast<dxcw::target>(entry.export_targets[i]);
            exports[i].entrypoint = entry.exports_entrypoints[i];
        }

        auto const success = dxcw::compile_library(compiler, entry.pathin_absolute, exports, entry.pathout_absolute, base_path_fs.c_str(), scratch_alloc);


        if (success)
            DXCW_LOG("compiled library {} ({} exports)", entry.pathin, entry.num_exports);
        else
            DXCW_LOG_WARN("error compiling library {} ({} exports)", entry.pathin, entry.num_exports);

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
        bool success = dxcw::parse_shaderlist_json(shaderlist_json, {}, num_shaders, {}, num_libraries, scratch_alloc);
        if (!success)
            return false;

        watch_binary_entries.resize(num_shaders);
        watch_binary_aux.resize(num_shaders);

        watch_library_entries.resize(num_libraries);
        watch_library_aux.resize(num_libraries);

        dxcw::parse_shaderlist_json(shaderlist_json, watch_binary_entries, num_shaders, watch_library_entries, num_libraries, scratch_alloc);
        for (auto i = 0u; i < num_shaders; ++i)
        {
            // recreate main watch flag
            watch_binary_aux[i].main_flag = dxcw::FileWatch::watchFile(watch_binary_entries[i].pathin_absolute);
            // refresh include flags
            f_refresh_includes(watch_binary_aux[i], watch_binary_entries[i].pathin_absolute);
            // compile
            f_compile_entry(watch_binary_entries[i]);
        }
        for (auto i = 0u; i < num_libraries; ++i)
        {
            // recreate main watch flag
            watch_library_aux[i].main_flag = dxcw::FileWatch::watchFile(watch_library_entries[i].pathin_absolute);
            // refresh include flags
            f_refresh_includes(watch_library_aux[i], watch_library_entries[i].pathin_absolute);
            // compile
            f_compile_library(watch_library_entries[i]);
        }

        return true;
    };

    bool const initial_success = f_refresh_all_entries();
    if (!initial_success)
    {
        // shouldn't happen because of the canonical path
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", base_path_fs.c_str());
        return 1;
    }

    DXCW_LOG("watching shaderlist json file at {}", base_path_fs.c_str());
    // clear initial watch
    shaderlist_watch->clear();

    std::signal(SIGINT, interrupt_handler);
    while (gv_keep_running)
    {
        // sleep
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(250ms);

        if (shaderlist_watch->isChanged())
        {
            DXCW_LOG("shaderlist json file changed, recompiling all shaders");

            // changes in shaderlist - refresh
            if (!f_refresh_all_entries())
            {
                DXCW_LOG_ERROR("shaderlist json file not readable after changes, aborting");
                return 1;
            }

            shaderlist_watch->clear();

            continue;
        }

        // poll shaders
        for (auto i = 0u; i < num_shaders; ++i)
        {
            auto& entry = watch_binary_aux[i];

            if (entry.main_flag->isChanged())
            {
                // main file changed, refresh includes and recompile
                f_refresh_includes(watch_binary_aux[i], watch_binary_entries[i].pathin_absolute);
                f_compile_entry(watch_binary_entries[i]);
                entry.main_flag->clear();
            }
            else
            {
                for (auto j = 0u; j < entry.num_files; ++j)
                {
                    if (entry.include_flags[j]->isChanged())
                    {
                        // single include changed, refresh includes and recompile
                        f_refresh_includes(entry, watch_binary_entries[i].pathin_absolute);
                        f_compile_entry(watch_binary_entries[i]);
                        break;
                    }
                }
            }
        }

        // poll libraries
        for (auto i = 0u; i < num_libraries; ++i)
        {
            auto& entry = watch_library_aux[i];

            if (entry.main_flag->isChanged())
            {
                // main file changed, refresh includes and recompile
                f_refresh_includes(watch_library_aux[i], watch_library_entries[i].pathin_absolute);
                f_compile_library(watch_library_entries[i]);
                entry.main_flag->clear();
            }
            else
            {
                for (auto j = 0u; j < entry.num_files; ++j)
                {
                    if (entry.include_flags[j]->isChanged())
                    {
                        // single include changed, refresh includes and recompile
                        f_refresh_includes(entry, watch_library_entries[i].pathin_absolute);
                        f_compile_library(watch_library_entries[i]);
                        break;
                    }
                }
            }
        }
    }

    compiler.destroy();
    DXCW_LOG("stopped watching");
    return 0;
}
