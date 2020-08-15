#pragma once

#include <cstddef>
#include <cstdint>

struct IDxcBlob;
struct IDxcLibrary;
struct IDxcCompiler3;
struct IDxcIncludeHandler;

namespace dxcw
{
struct binary
{
    binary() = default;
    binary(IDxcBlob* blob);

    IDxcBlob* internal_blob = nullptr;
    std::byte const* data = nullptr;
    size_t size = 0;
};

enum class target
{
    vertex,
    hull,
    domain,
    geometry,
    pixel,

    compute
};

enum class output
{
    dxil,
    spirv
};

void destroy_blob(IDxcBlob* blob);

inline void destroy(binary const& b) { destroy_blob(b.internal_blob); }

struct compiler
{
public:
    void initialize();
    void destroy();

    ///
    /// \brief compile_binary
    /// \param raw_text                         - the HLSL code (ascii text)
    /// \param entrypoint                       - name of the entrypoint function
    /// \param target                           - shader stage
    /// \param output                           - output format, DXIL (D3D12) or SPIR-V (Vulkan)
    /// \param opt_additional_include_paths     - additional paths used for #include directive resolution (optional)
    /// \param build_debug_info                 - whether to build debug information (.pdb, -Zi flag)
    /// \param opt_filename_for_errors          - filename that is logged if errors occur during compilation (optional)
    /// \param opt_defines                      - defines as a comma separated list (ex.: "MYVAL=1,WITH_IBL=0")
    /// \return binary data, must be freed using dxcw::destroy
    ///
    [[nodiscard]] binary compile_binary(char const* raw_text,
                                        char const* entrypoint,
                                        target target,
                                        output output,
                                        bool build_debug_info = false,
                                        char const* opt_additional_include_paths = nullptr,
                                        char const* opt_filename_for_errors = nullptr,
                                        char const* opt_defines = nullptr);

private:
    IDxcLibrary* _lib = nullptr;
    IDxcCompiler3* _compiler = nullptr;
    IDxcIncludeHandler* _include_handler = nullptr;
};
}
