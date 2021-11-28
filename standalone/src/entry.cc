#include "entry.hh"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <clean-core/alloc_array.hh>
#include <clean-core/alloc_vector.hh>

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

int dxcw::display_version_and_exit()
{
    dxcw::compiler compiler;
    compiler.initialize();

    bool success = compiler.print_version();

    compiler.destroy();
    return success ? 0 : 1;
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
    compiler.print_version();

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

int dxcw::compile_shaderlist_watch(const char* shaderlist_path, cc::allocator* scratch_alloc)
{
    dxcw::compiler compiler;
    compiler.initialize();
    compiler.print_version();

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
        cc::alloc_vector<dxcw::fixed_string> included_files;
        bool was_last_compilation_successful = true;
    };

    std::vector<dxcw::shaderlist_binary_entry_owning> watch_entries;
    std::vector<auxilliary_watch_entry> watch_aux;

    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_path).remove_filename(), ec).string();
    char const* additional_includes[] = {base_path_fs.c_str()};

    if (ec)
    {
        DXCW_LOG_ERROR("failed to open shaderlist file at {}", shaderlist_path);
        return 1;
    }

    auto const f_refresh_includes = [&](auxilliary_watch_entry& aux_entry, char const* shader_path) -> void
    {
        aux_entry.included_files = dxcw::parse_includes(shader_path, additional_includes);

        for (auto j = 0u; j < aux_entry.included_files.size(); ++j)
        {
            aux_entry.include_flags[j] = dxcw::FileWatch::watchFile(aux_entry.included_files[j].str);
        }
        for (auto j = aux_entry.included_files.size(); j < sc_max_num_includes; ++j)
        {
            aux_entry.include_flags[j] = nullptr;
        }
    };

    auto const f_refresh_all_entries = [&]() -> bool
    {
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
            dxcw::compile_binary_entry(compiler, watch_entries[i], additional_includes, scratch_alloc);
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
                dxcw::compile_binary_entry(compiler, watch_entries[i], additional_includes, scratch_alloc);
                entry.main_flag->clear();
            }
            else
            {
                for (auto j = 0u; j < entry.included_files.size(); ++j)
                {
                    if (entry.include_flags[j]->isChanged())
                    {
                        // single include changed, refresh includes and recompile
                        f_refresh_includes(entry, watch_entries[i].pathin_absolute);
                        dxcw::compile_binary_entry(compiler, watch_entries[i], additional_includes, scratch_alloc);
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

int dxcw::compile_shaderlist_json_single(const char* shaderlist_json, cc::allocator* scratch_alloc)
{
    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_json).remove_filename(), ec);
    if (ec)
    {
        DXCW_LOG_ERROR("failed to make path canonical for shaderlist json file at {}", shaderlist_json);
        return 1;
    }
    auto const base_path_string = base_path_fs.string();
    char const* additional_includes[] = {base_path_string.c_str()};

    dxcw::compiler compiler;
    compiler.initialize();
    compiler.print_version();

    cc::alloc_vector<dxcw::shaderlist_binary_entry_owning> watch_binary_entries(scratch_alloc);
    cc::alloc_vector<dxcw::shaderlist_library_entry_owning> watch_library_entries(scratch_alloc);

    // parse the shaderlist
    unsigned num_shaders, num_libraries;
    bool not_enough_space = false;
    do
    {
        bool success = dxcw::parse_shaderlist_json(shaderlist_json, watch_binary_entries, num_shaders, watch_library_entries, num_libraries, scratch_alloc);

        if (!success)
            return false;

        // the vectors might currently not have enough space for all entries
        not_enough_space = (num_shaders > watch_binary_entries.size()) || (num_libraries > watch_library_entries.size());

        if (not_enough_space)
        {
            // resize the vectors to make enough space for a re-run
            watch_binary_entries.resize(num_shaders);
            watch_library_entries.resize(num_libraries);
        }
    } while (not_enough_space); // do-while because this could theoretically happen multiple times with unlucky file changes between each run

    unsigned num_errors = 0;
    for (auto i = 0u; i < num_shaders; ++i)
    {
        bool ok = dxcw::compile_binary_entry(compiler, watch_binary_entries[i], additional_includes, scratch_alloc);
        if (!ok)
            ++num_errors;
    }
    for (auto i = 0u; i < num_libraries; ++i)
    {
        bool ok = dxcw::compile_library_entry(compiler, watch_library_entries[i], additional_includes, scratch_alloc);
        if (!ok)
            ++num_errors;
    }


    compiler.destroy();

    DXCW_LOG("compiled {} shaders, {} libraries, {} errors", num_shaders, num_libraries, num_errors);
    return (num_errors == 0) ? 0 : 1;
}

int dxcw::compile_shaderlist_json_watch(const char* shaderlist_json_path, cc::allocator* scratch_alloc)
{
    dxcw::compiler compiler;
    compiler.initialize();
    compiler.print_version();

    dxcw::FileWatch::SharedFlag shaderlist_watch = dxcw::FileWatch::watchFile(shaderlist_json_path);
    if (!shaderlist_watch)
    {
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", shaderlist_json_path);
        return 1;
    }

    constexpr unsigned sc_max_num_includes = 32;
    struct auxilliary_watch_entry
    {
        dxcw::FileWatch::SharedFlag main_flag;
        dxcw::FileWatch::SharedFlag include_flags[sc_max_num_includes];
        cc::alloc_vector<dxcw::fixed_string> included_files;
        bool was_last_compilation_successful = true;
    };

    unsigned num_shaders = 0;
    cc::alloc_vector<dxcw::shaderlist_binary_entry_owning> watch_binary_entries(scratch_alloc);
    cc::alloc_vector<auxilliary_watch_entry> watch_binary_aux(scratch_alloc);

    unsigned num_libraries = 0;
    cc::alloc_vector<dxcw::shaderlist_library_entry_owning> watch_library_entries(scratch_alloc);
    cc::alloc_vector<auxilliary_watch_entry> watch_library_aux(scratch_alloc);

    unsigned num_binary_errors = 0, num_library_errors = 0;
    bool any_errors_remaining = false;

    std::error_code ec;
    auto const base_path_fs = std::filesystem::canonical(std::filesystem::path(shaderlist_json_path).remove_filename(), ec).string();
    char const* additional_includes[] = {base_path_fs.c_str()};

    if (ec)
    {
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", shaderlist_json_path);
        return 1;
    }

    auto f_refresh_includes = [&](auxilliary_watch_entry& aux_entry, char const* shader_path) -> void
    {
        aux_entry.included_files = dxcw::parse_includes(shader_path, additional_includes);

        for (auto j = 0u; j < aux_entry.included_files.size(); ++j)
        {
            // DXCW_LOG("parsed include #{} for aux {}: {}", j, shader_path, aux_entry.include_entries[j].includepath_absolute);
            aux_entry.include_flags[j] = dxcw::FileWatch::watchFile(aux_entry.included_files[j].str);
        }
        for (auto j = aux_entry.included_files.size(); j < sc_max_num_includes; ++j)
        {
            aux_entry.include_flags[j] = nullptr;
        }
    };

    auto f_refresh_all_entries = [&]() -> bool
    {
        // parse the shaderlist
        bool not_enough_space = false;
        do
        {
            bool success = dxcw::parse_shaderlist_json(shaderlist_json_path, watch_binary_entries, num_shaders, watch_library_entries, num_libraries, scratch_alloc);

            if (!success)
                return false;

            // the vectors might currently not have enough space for all entries
            not_enough_space = (num_shaders > watch_binary_entries.size()) || (num_libraries > watch_library_entries.size());

            if (not_enough_space)
            {
                // resize the vectors to make enough space for a re-run
                watch_binary_entries.resize(num_shaders);
                watch_library_entries.resize(num_libraries);
            }
        } while (not_enough_space); // do-while because this could theoretically happen multiple times with unlucky file changes between each run

        // resize the aux vectors to always fit exactly
        watch_binary_aux.resize(num_shaders);
        watch_library_aux.resize(num_libraries);
        // (do not resize (downsize) the main vectors, not required as they are never looped)

        DXCW_LOG("parsed json file, detected {} binaries, {} libraries", num_shaders, num_libraries);

        num_binary_errors = 0;
        num_library_errors = 0;
        any_errors_remaining = false;

        for (auto i = 0u; i < num_shaders; ++i)
        {
            // recreate main watch flag
            watch_binary_aux[i].main_flag = dxcw::FileWatch::watchFile(watch_binary_entries[i].pathin_absolute);
            // refresh include flags
            f_refresh_includes(watch_binary_aux[i], watch_binary_entries[i].pathin_absolute);
            // compile
            {
                auto const& entry = watch_binary_entries[i];

                DXCW_LOG("  [B {}/{}] building {} ({}; {})", i + 1, num_shaders, entry.pathin, entry.target, entry.entrypoint);
                auto const success = dxcw::compile_shader(compiler, entry.pathin_absolute, entry.target, entry.entrypoint, entry.pathout_absolute,
                                                          additional_includes, scratch_alloc);

                watch_binary_aux[i].was_last_compilation_successful = success;

                if (!success)
                {
                    ++num_binary_errors;
                }
            }
        }
        for (auto i = 0u; i < num_libraries; ++i)
        {
            // recreate main watch flag
            watch_library_aux[i].main_flag = dxcw::FileWatch::watchFile(watch_library_entries[i].pathin_absolute);
            // refresh include flags
            f_refresh_includes(watch_library_aux[i], watch_library_entries[i].pathin_absolute);
            // compile
            {
                auto const& entry = watch_library_entries[i];
                auto exports = cc::alloc_array<library_export>::uninitialized(entry.num_exports, scratch_alloc);

                for (auto i = 0u; i < entry.num_exports; ++i)
                {
                    exports[i].internal_name = entry.exports_internal_names[i];
                    exports[i].export_name = entry.exports_exported_names[i];
                }

                DXCW_LOG("  [L {}/{}] building library {} ({} exports)", i + 1, num_libraries, entry.pathin, entry.num_exports);

                auto const success = dxcw::compile_library(compiler, entry.pathin_absolute, exports, entry.pathout_absolute, additional_includes, scratch_alloc);

                watch_library_aux[i].was_last_compilation_successful = success;

                if (!success)
                {
                    ++num_library_errors;
                }
            }
        }

        return true;
    };

    auto f_output_pending_error_message = [&]
    {
        if (num_binary_errors + num_library_errors > 0)
        {
            DXCW_LOG_WARN("files with errors remaining ({} binaries, {} libraries)", num_binary_errors, num_library_errors);
            any_errors_remaining = true;
        }
        else
        {
            if (any_errors_remaining)
            {
                DXCW_LOG("all remaining errors resolved");
                any_errors_remaining = false;
            }
        }
    };
    auto f_refresh_binary = [&](unsigned index) -> bool
    {
        // refresh includes
        f_refresh_includes(watch_binary_aux[index], watch_binary_entries[index].pathin_absolute);

        // compile
        auto const& entry = watch_binary_entries[index];

        char const* additional_includes[] = {base_path_fs.c_str()};

        DXCW_LOG("rebuilding {} ({}; {})", entry.pathin, entry.target, entry.entrypoint);
        auto const success = dxcw::compile_shader(compiler, entry.pathin_absolute, entry.target, entry.entrypoint, entry.pathout_absolute,
                                                  additional_includes, scratch_alloc);

        bool const prev_success = watch_binary_aux[index].was_last_compilation_successful;

        if (prev_success && !success)
        {
            ++num_binary_errors;
        }
        else if (!prev_success && success)
        {
            CC_ASSERT(num_binary_errors > 0 && "programmer errror");
            --num_binary_errors;
        }

        watch_binary_aux[index].was_last_compilation_successful = success;
        return success;
    };

    auto f_refresh_library = [&](unsigned index) -> bool
    {
        auto const& entry = watch_library_entries[index];
        auto& entry_aux = watch_library_aux[index];

        f_refresh_includes(entry_aux, entry.pathin_absolute);

        auto exports = cc::alloc_array<library_export>::uninitialized(entry.num_exports, scratch_alloc);

        for (auto i = 0u; i < entry.num_exports; ++i)
        {
            exports[i].internal_name = entry.exports_internal_names[i];
            exports[i].export_name = entry.exports_exported_names[i];
        }

        DXCW_LOG("rebuilding {} ({} exports)", entry.pathin, entry.num_exports);

        char const* additional_includes[] = {base_path_fs.c_str()};
        bool const success = dxcw::compile_library(compiler, entry.pathin_absolute, exports, entry.pathout_absolute, additional_includes, scratch_alloc);

        bool const prev_success = entry_aux.was_last_compilation_successful;

        if (prev_success && !success)
        {
            ++num_library_errors;
        }
        else if (!prev_success && success)
        {
            CC_ASSERT(num_library_errors > 0 && "programmer errror");
            --num_library_errors;
        }

        entry_aux.was_last_compilation_successful = success;
        return success;
    };

    bool const initial_success = f_refresh_all_entries();

    if (!initial_success)
    {
        // shouldn't happen because of the canonical path
        DXCW_LOG_ERROR("failed to open shaderlist json file at {}", base_path_fs.c_str());
        return 1;
    }

    // clear initial watch
    shaderlist_watch->clear();
    // install handler for ctrl+c detection
    std::signal(SIGINT, interrupt_handler);

    DXCW_LOG("watching shaderlist json file at {}", base_path_fs.c_str());
    f_output_pending_error_message();

    while (gv_keep_running)
    {
        // sleep
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(250ms);

        if (shaderlist_watch->isChanged())
        {
            DXCW_LOG("shaderlist json file changed");

            // changes in shaderlist - refresh
            if (!f_refresh_all_entries())
            {
                DXCW_LOG_ERROR("shaderlist json file not readable after changes, aborting");
                return 1;
            }

            shaderlist_watch->clear();
            f_output_pending_error_message();

            continue;
        }

        // poll shaders
        bool any_changes_in_shaders = false;
        for (auto i = 0u; i < num_shaders; ++i)
        {
            auto& entry = watch_binary_aux[i];

            if (entry.main_flag->isChanged())
            {
                any_changes_in_shaders = true;

                // main file changed, refresh includes and recompile
                f_refresh_binary(i);
                entry.main_flag->clear();
            }
            else
            {
                for (auto j = 0u; j < entry.included_files.size(); ++j)
                {
                    if (entry.include_flags[j]->isChanged())
                    {
                        any_changes_in_shaders = true;

                        // single include changed, refresh includes and recompile
                        f_refresh_binary(i);
                        break;
                    }
                }
            }
        }


        // poll libraries
        bool any_changes_in_libraries = false;
        for (auto i = 0u; i < num_libraries; ++i)
        {
            auto& entry = watch_library_aux[i];

            if (entry.main_flag->isChanged())
            {
                any_changes_in_libraries = true;
                // main file changed, refresh includes and recompile
                f_refresh_library(i);
                entry.main_flag->clear();
            }
            else
            {
                for (auto j = 0u; j < entry.included_files.size(); ++j)
                {
                    if (entry.include_flags[j]->isChanged())
                    {
                        any_changes_in_libraries = true;
                        // single include changed, refresh includes and recompile
                        f_refresh_library(i);
                        break;
                    }
                }
            }
        }

        if (any_changes_in_libraries || any_changes_in_shaders)
        {
            f_output_pending_error_message();
        }
    }

    compiler.destroy();
    DXCW_LOG("stopped watching");
    return 0;
}
