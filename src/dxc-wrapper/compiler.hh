#pragma once

#include <cstddef>
#include <cstdint>

#include <clean-core/fwd.hh>

struct IDxcBlob;
struct IDxcLibrary;
struct IDxcCompiler3;
struct IDxcContainerReflection;
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

void destroy_blob(IDxcBlob* blob);
void destroy(binary const& b);

enum class target : uint8_t
{
    vertex,
    hull,
    domain,
    geometry,
    pixel,

    compute,

    raygeneration,
    intersection,
    anyhit,
    closesthit,
    miss,

    callable,
    mesh,
    amplification
};

enum class output : uint8_t
{
    dxil,
    spirv
};

struct library_export
{
    char const* internal_name = nullptr; ///< name of the export inside HLSL, required
    char const* export_name = nullptr;   ///< name of the export as it will be visible in the binary (same as internal if nullptr)
};

struct compiler
{
public:
    void initialize();
    void destroy();

    ///
    /// \brief compiles HLSL code to a DXIL or SPIR-V shader binary
    /// \param raw_text                         - the HLSL code (ascii text)
    /// \param entrypoint                       - name of the entrypoint function
    /// \param target                           - shader stage
    /// \param output                           - output format, DXIL (D3D12) or SPIR-V (Vulkan)
    /// \param build_debug_info                 - whether to build debug information (.pdb, -Zi flag)
    /// \param opt_additional_include_paths     - additional paths used for #include directive resolution (optional)
    /// \param opt_filename_for_errors          - filename that is logged if errors occur during compilation (optional)
    /// \param opt_defines                      - defines as a comma separated list (ex.: "MYVAL=1,WITH_IBL=0") (optional)
    /// \param scratch_alloc                    - allocator used for scratch memory required during compilation
    /// \return binary data, can outlive compiler, must be freed using dxcw::destroy
    ///
    [[nodiscard]] binary compile_shader(char const* raw_text,
                                        char const* entrypoint,
                                        target target,
                                        output output,
                                        bool build_debug_info = false,
                                        char const* opt_additional_include_paths = nullptr,
                                        char const* opt_filename_for_errors = nullptr,
                                        char const* opt_defines = nullptr,
                                        cc::allocator* scratch_alloc = cc::system_allocator);

    ///
    /// \brief compiles HLSL code to a DXIL or SPIR-V library binary
    /// \param raw_text                         - the HLsL code (ascii text)
    /// \param exports                          - internal and exported name per export
    /// \param output                           - output format, DXIL (D3D12) or SPIR-V (Vulkan)
    /// \param build_debug_info                 - whether to build debug information (.pdb, -Zi flag)
    /// \param opt_additional_include_paths     - additional paths used for #include directive resolution (optional)
    /// \param opt_filename_for_errors          - filename that is logged if errors occur during compilation (optional)
    /// \param opt_defines                      - defines as a comma separated list (ex.: "MYVAL=1,WITH_IBL=0") (optional)
    /// \param scratch_alloc                    - allocator used for scratch memory required during compilation
    /// \return binary data, can outlive compiler, must be freed using dxcw::destroy
    ///
    [[nodiscard]] binary compile_library(char const* raw_text,
                                         cc::span<library_export const> exports,
                                         output output,
                                         bool build_debug_info = false,
                                         char const* opt_additional_include_paths = nullptr,
                                         char const* opt_filename_for_errors = nullptr,
                                         char const* opt_defines = nullptr,
                                         cc::allocator* scratch_alloc = cc::system_allocator);


    /// retreives the loaded DXC major and minor version
    bool get_version(unsigned& out_major, unsigned& out_minor) const;


    ///
    /// \brief retreives the loaded DXC commit number and hash
    /// \param out_num_commits                  - commit number
    /// \param out_commit_hash                  - commit hash, ptr valid for lifetime of compiler
    /// \return
    ///
    bool get_version_commit(unsigned& out_num_commits, char const*& out_commit_hash) const;

    /// prints the loaded DXC version and commit if applicable
    bool print_version() const;

private:
    IDxcLibrary* _lib = nullptr;
    IDxcCompiler3* _compiler = nullptr;
    // IDxcContainerReflection* _reflection = nullptr;
    IDxcIncludeHandler* _include_handler = nullptr;
};
}
