#pragma once

#include <cstddef>
#include <cstdint>

#include <clean-core/fwd.hh>
#include <clean-core/span.hh>

#include <dxc-wrapper/common/api.hh>

struct IDxcBlob;
struct IDxcLibrary;
struct IDxcUtils;
struct IDxcCompiler3;
struct IDxcContainerReflection;
struct IDxcIncludeHandler;
struct IDxcResult;
struct IDxcBlobUtf8;
typedef struct _D3D12_SHADER_DESC D3D12_SHADER_DESC;

namespace dxcw
{
struct DXCW_API binary
{
    binary() = default;
    binary(IDxcBlob* blob);

    IDxcBlob* internal_blob = nullptr;
    std::byte const* data = nullptr;
    size_t size = 0;
};

DXCW_API void destroy_blob(IDxcBlob* blob);
DXCW_API void destroy_result(IDxcResult* blob);
DXCW_API void destroy(binary const& b);

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

enum class shader_model
{
    sm_6_0 = 0,
    sm_6_1,
    sm_6_2,
    sm_6_3,
    sm_6_4,
    sm_6_5,
    sm_6_6,

    sm_use_default = 255
};

struct library_export
{
    char const* internal_name = nullptr; ///< name of the export inside HLSL, required
    char const* export_name = nullptr;   ///< name of the export as it will be visible in the binary (same as internal if nullptr)
};

struct shader_description
{
    // the HLSL code (ascii text)
    char const* raw_text = nullptr;
    // name of the entrypoint function
    char const* entrypoint = nullptr;
    // shader stage
    target target = target::vertex;
    shader_model sm = shader_model::sm_use_default;
};

struct library_description
{
    // the HLSL code (ascii text)
    char const* raw_text = nullptr;
    // internal and exported name per export
    cc::span<library_export const> exports = {};
};

struct compilation_config
{
    // output format, DXIL (D3D12) or SPIR-V (Vulkan)
    output output_format = output::dxil;
    // disable optimizations (-Od) and embed PDB information into binary (-Zi, -Qembed_debug)
    bool build_debug = false;

    // additional paths used for #include directive resolution (optional)
    cc::span<char const* const> additional_include_paths = {};
    // defines (ex.: "MYVAL=1", "WITH_IBL=0", "HAS_EMISSIVE") (optional)
    cc::span<char const* const> defines = {};
    // filename that is logged if errors occur during compilation (optional)
    char const* filename_for_errors = nullptr;
};

struct DXCW_API compiler
{
public:
    void initialize();
    void destroy();

    // Advanced API: Retrieve a IDxcResult* for detailed interaction
    IDxcResult* compile_shader_result(shader_description const& shader, compilation_config const& config, cc::allocator* scratch_alloc = cc::system_allocator);

    // Advanced API: Retrieve a IDxcResult* for detailed interaction
    IDxcResult* compile_library_result(library_description const& library, compilation_config const& config, cc::allocator* scratch_alloc = cc::system_allocator);

    // Returns true if the compilation succeeded
    bool is_result_successful(IDxcResult* result);

    // Extracts the binary from the result
    bool get_result_binary(IDxcResult* result, binary* out_binary);

    // Extracts an error string, returns true if errors / warnings occured
    // If this returns true, must call free_result_error_blob on *out_blob_to_free
    bool get_result_error_string(IDxcResult* result, IDxcBlobUtf8** out_blob_to_free, char** out_error_string);

    void free_result_error_blob(IDxcBlobUtf8* blob_to_free);

    // Extracts reflection data from the result (include <d3d12shader.h> for the struct, Windows only)
    bool get_result_reflection(IDxcResult* result, D3D12_SHADER_DESC* out_shader_desc);


    ///
    /// \brief compiles HLSL code to a DXIL or SPIR-V shader binary
    /// \param raw_text                         - the HLSL code (ascii text)
    /// \param entrypoint                       - name of the entrypoint function
    /// \param target                           - shader stage
    /// \param output                           - output format, DXIL (D3D12) or SPIR-V (Vulkan)
    /// \param build_debug                      - disable optimizations (-Od) and embed PDB information into binary (-Zi, -Qembed_debug)
    /// \param opt_additional_include_paths     - additional paths used for #include directive resolution (optional)
    /// \param opt_filename_for_errors          - filename that is logged if errors occur during compilation (optional)
    /// \param opt_defines                      - defines (ex.: "MYVAL=1", "WITH_IBL=0", "HAS_EMISSIVE") (optional)
    /// \param scratch_alloc                    - allocator used for scratch memory required during compilation
    /// \return binary data, can outlive compiler, must be freed using dxcw::destroy
    ///
    [[nodiscard]] binary compile_shader(char const* raw_text,
        char const* entrypoint,
        target target,
        output output,
        shader_model sm = shader_model::sm_use_default,
        bool build_debug = false,
        cc::span<char const* const> opt_additional_include_paths = {},
        char const* opt_filename_for_errors = nullptr,
        cc::span<char const* const> opt_defines = {},
        cc::allocator* scratch_alloc = cc::system_allocator);

    ///
    /// \brief compiles HLSL code to a DXIL or SPIR-V library binary
    /// \param raw_text                         - the HLSL code (ascii text)
    /// \param exports                          - internal and exported name per export
    /// \param output                           - output format, DXIL (D3D12) or SPIR-V (Vulkan)
    /// \param build_debug                      - disable optimizations (-Od) and embed PDB information into binary (-Zi, -Qembed_debug)
    /// \param opt_additional_include_paths     - additional paths used for #include directive resolution (optional)
    /// \param opt_filename_for_errors          - filename that is logged if errors occur during compilation (optional)
    /// \param opt_defines                      - defines (ex.: "MYVAL=1", "WITH_IBL=0", "HAS_EMISSIVE") (optional)
    /// \param scratch_alloc                    - allocator used for scratch memory required during compilation
    /// \return binary data, can outlive compiler, must be freed using dxcw::destroy
    ///
    [[nodiscard]] binary compile_library(char const* raw_text,
                                         cc::span<library_export const> exports,
                                         output output,
                                         bool build_debug = false,
                                         cc::span<char const* const> opt_additional_include_paths = {},
                                         char const* opt_filename_for_errors = nullptr,
                                         cc::span<char const* const> opt_defines = {},
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

    static shader_model get_default_shader_model();

    IDxcLibrary* _lib = nullptr;
    IDxcCompiler3* _compiler = nullptr;
    IDxcUtils* _utils = nullptr;
    IDxcContainerReflection* _reflection = nullptr;
    IDxcIncludeHandler* _include_handler = nullptr;
};
}
