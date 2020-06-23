#include <nexus/args.hh>

#include <rich-log/logger.hh>

#include "entry.hh"

int main(int argc, char const* argv[])
{
    rlog::enable_win32_colors();
   // rlog::set_console_log_style(rlog::console_log_style::brief);

    bool is_watch_mode = false;
    cc::string shaderlist_file;
    auto args = nx::args("dxcw-standalone", "standalone CLI for dxc-wrapper, compiles HLSL to DXIL (D3D12) or SPIR-V (Vulkan)")
                    .add(is_watch_mode, {"w", "watch"}, "listen for changes and recompile")
                    .add(shaderlist_file, {"l", "list"}, "parse a shaderlist and compile all shaders within instead of a single file");

    if (!args.parse(argc, argv))
        return 1;

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
    else
    {
        return dxcw::compile_shader_single(args);
    }
}
