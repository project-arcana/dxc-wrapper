#include <nexus/args.hh>

#include <rich-log/logger.hh>

#include <dxc-wrapper/common/log.hh>

#include "entry.hh"

int main(int argc, char const* argv[])
{
    rlog::enable_win32_colors();
    rlog::set_console_log_style(rlog::console_log_style::brief);

    bool is_watch_mode = false;
    bool is_display_version_mode = false;
    cc::string shaderlist_file;
    cc::string json_file;
    auto args = nx::args("dxcw-standalone", "standalone CLI for dxc-wrapper, compiles HLSL to DXIL (D3D12) or SPIR-V (Vulkan)\n\n"
                                            "Usage:\n"
                                            "./dxcw [input file] [entrypoint] [target] [output file without ending]\n"
                                            "  target is one of: vs, ds, hs, gs, ps, cs\n\n"
                                            "or: ./dxcw -l [list file]\n"
                                            "  list file contains normal arguments line-by-line\n"
                                            "or: ./dxcw -w -l [list file]\n"
                                            "  watch mode - list file contains normal arguments line-by-line\n")
                    .add(is_display_version_mode, {"v", "version"}, "display DXC version and exit")
                    .add(is_watch_mode, {"w", "watch"}, "listen for changes and recompile")
                    .add(shaderlist_file, {"l", "list"}, "parse a shaderlist and compile all shaders within instead of a single file")
                    .add(json_file, {"j", "json"}, "parse a shaderlist json and compile all shaders within");

    if (!args.parse(argc, argv))
    {
        return 1;
    }

    if (is_display_version_mode)
    {
        return dxcw::display_version_and_exit();
    }

    if (shaderlist_file.size() > 0)
    {
        if (is_watch_mode)
        {
            return dxcw::compile_shaderlist_watch(shaderlist_file.c_str());
        }
        else
        {
            return dxcw::compile_shaderlist_single(shaderlist_file.c_str());
        }
    }
    else if (json_file.size() > 0)
    {
        if (is_watch_mode)
        {
            return dxcw::compile_shaderlist_json_watch(json_file.c_str());
        }
        else
        {
            return dxcw::compile_shaderlist_json_single(json_file.c_str());
        }
    }
    else if (args.positional_args().size() == 4)
    {
        auto const res = dxcw::compile_shader_single(args);

        if (is_watch_mode)
        {
            DXCW_LOG_ERROR("cannot watch single shader compilation");
        }

        return res;
    }
    else
    {
        DXCW_LOG_ERROR("invalid arguments, run ./dxcw -h for usage");
        return 1;
    }
}
