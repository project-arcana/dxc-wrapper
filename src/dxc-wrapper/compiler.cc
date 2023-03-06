#include "compiler.hh"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#ifdef DXCW_HAS_OPTICK
#include <optick.h>
#endif

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

// clang-format off
//#include <clean-core/native/detail/win32_sanitize_before.inl> // WIN32_LEAN_AND_MEAN kills features we require here

struct IUnknown;
#pragma warning(push, 0)

#include <Windows.h>

#include <d3d12shader.h> // FOR D3D12_SHADER_DESC

#include <clean-core/native/detail/win32_sanitize_after.inl> // this is still reasonable, (undef min, max, etc.)
// clang-format on

#else

#ifdef CC_COMPILER_POSIX

// DXC assumes that clang has support for UUIDs, Clang 7 at least does not have it
// this define is used in dxc/Support/WinAdapter.h

#ifdef CC_COMPILER_CLANG
// suppress clang warning about __EMULATE_UUID being a reserved macro ID
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreserved-id-macro"
#endif

#define __EMULATE_UUID 1

#ifdef CC_COMPILER_CLANG
#pragma GCC diagnostic pop
#endif

#endif

#endif

#include <dxc/dxcapi.h>

#include <clean-core/alloc_array.hh>
#include <clean-core/array.hh>
#include <clean-core/assert.hh>
#include <clean-core/capped_vector.hh>
#include <clean-core/defer.hh>
#include <clean-core/native/wchar_conversion.hh>

#include "common/log.hh"

#define DXCW_STR(S) #S
#define DXCW_XSTR(S) DXCW_STR(S)

#define DXCW_DEFAULT_SHADER_MODEL 6_5
#define DXCW_DEFAULT_SHADER_MODEL_ENUM CC_MACRO_JOIN(dxcw::shader_model::sm_, DXCW_DEFAULT_SHADER_MODEL)
#define DXCW_DEFAULT_SHADER_MODEL_STR DXCW_XSTR(DXCW_DEFAULT_SHADER_MODEL)

namespace
{
int get_shader_model_minor_version(dxcw::shader_model sm)
{
    if (sm == dxcw::shader_model::sm_use_default)
    {
        sm = DXCW_DEFAULT_SHADER_MODEL_ENUM;
    }

    return (int)sm;
}

char get_shader_profile_char(dxcw::target target)
{
    using ct = dxcw::target;
    switch (target)
    {
    case ct::vertex:
        return 'v';
    case ct::hull:
        return 'h';
    case ct::domain:
        return 'd';
    case ct::geometry:
        return 'g';
    case ct::pixel:
        return 'p';
    case ct::compute:
        return 'c';
    case ct::mesh:
        return 'm';
    case ct::amplification:
        return 'a';
    default:
        CC_UNREACHABLE("Unknown shader target");
        return 'X';
    }
}

char const* get_output_type_literal(dxcw::output output) { return (output == dxcw::output::dxil) ? "DXIL" : "SPIR-V"; }

[[maybe_unused]] wchar_t const* get_library_export_name(dxcw::target tgt, unsigned& out_strlen)
{
    using ct = dxcw::target;
#define DXCW_CASE_RETURN(_val_)         \
    case ct::_val_:                     \
        out_strlen = sizeof #_val_ - 1; \
        return L## #_val_
    switch (tgt)
    {
        DXCW_CASE_RETURN(compute);
        DXCW_CASE_RETURN(vertex);
        DXCW_CASE_RETURN(pixel);
        DXCW_CASE_RETURN(hull);
        DXCW_CASE_RETURN(domain);
        DXCW_CASE_RETURN(geometry);
        DXCW_CASE_RETURN(raygeneration);
        DXCW_CASE_RETURN(intersection);
        DXCW_CASE_RETURN(anyhit);
        DXCW_CASE_RETURN(closesthit);
        DXCW_CASE_RETURN(miss);
        DXCW_CASE_RETURN(callable);
        DXCW_CASE_RETURN(mesh);
        DXCW_CASE_RETURN(amplification);
    default:
        return L"ERR_UNKNOWN_TARGET";
    }

#undef DXCW_CASE_RETURN
}

void verify_hres(HRESULT hres) { CC_RUNTIME_ASSERT(SUCCEEDED(hres) && "DXC operation failed"); }

#define DEFER_RELEASE(_ptr_)  \
    CC_DEFER                  \
    {                         \
        if (_ptr_)            \
            _ptr_->Release(); \
    }

struct argument_memory
{
    cc::alloc_array<LPCWSTR> pointers;
    size_t num_arguments = 0;

    void initialize(size_t max_num_args, cc::allocator* alloc)
    {
        pointers = cc::alloc_array<LPCWSTR>::uninitialized(max_num_args, alloc);
        num_arguments = 0;
    }

    void add_arg(wchar_t const* str)
    {
        CC_ASSERT(num_arguments < pointers.size() && "write OOB");
        pointers[num_arguments] = str;
        ++num_arguments;
    }

    void add_multiple_args(wchar_t const* const* strs, size_t num)
    {
        CC_ASSERT(num_arguments + num <= pointers.size() && "write OOB");
        std::memcpy(pointers.data() + num_arguments, &strs[0], num * sizeof(wchar_t const*));
        num_arguments += num;
    }

    LPCWSTR* get_data()
    {
        if (num_arguments == 0)
        {
            return nullptr;
        }

        return pointers.data();
    }

    uint32_t get_num() const { return uint32_t(num_arguments); }
};

struct widechar_memory
{
    cc::alloc_array<wchar_t> buffer;
    size_t num_chars = 0;

    void initialize(size_t max_num_chars, cc::allocator* alloc)
    {
        buffer = cc::alloc_array<wchar_t>::uninitialized(max_num_chars, alloc);
        num_chars = 0;
    }

    wchar_t const* add_text(wchar_t const* text, size_t strlen)
    {
        CC_ASSERT(num_chars + strlen <= buffer.size() && "text write OOB");

        wchar_t* const res = buffer.data() + num_chars;
        std::memcpy(res, text, strlen * sizeof(wchar_t));
        num_chars += strlen;

        return res;
    }

    wchar_t const* convert_and_add_text(char const* text, int opt_num_src_chars = -1)
    {
        auto const destination_span = cc::span(buffer).subspan(num_chars, buffer.size() - num_chars);
        auto const num_wchars_written = cc::char_to_widechar(destination_span, text, opt_num_src_chars);
        num_chars += num_wchars_written + 1;

        return destination_span.data();
    }

    wchar_t const* add_profile_text(dxcw::target target, dxcw::shader_model sm)
    {
        char buf[128];
        // assemble a string like vs_6_6
        // first char is the "target" (shader stage)
        // last int is the minor SM version
        snprintf(buf, sizeof(buf), "%cs_6_%d", get_shader_profile_char(target), get_shader_model_minor_version(sm));
        return convert_and_add_text(buf);
    }
};
}

void dxcw::compiler::initialize()
{
#ifdef DXCW_HAS_OPTICK
    OPTICK_EVENT();
#endif

    CC_ASSERT(_lib == nullptr && "double initialize");
    verify_hres(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&_lib)));
    verify_hres(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&_compiler)));
    verify_hres(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&_utils)));
    verify_hres(_lib->CreateIncludeHandler(&_include_handler));
    // verify_hres(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&_reflection)));
}

void dxcw::compiler::destroy()
{
#ifdef DXCW_HAS_OPTICK
    OPTICK_EVENT();
#endif

    if (_lib == nullptr)
        return;

    if (_reflection)
    {
        _reflection->Release();
        _reflection = nullptr;
    }
    _include_handler->Release();
    _include_handler = nullptr;
    _compiler->Release();
    _compiler = nullptr;
    _utils->Release();
    _utils = nullptr;
    _lib->Release();
    _lib = nullptr;
}

dxcw::binary dxcw::compiler::compile_shader(const char* raw_text,
                                            const char* entrypoint,
                                            dxcw::target target,
                                            dxcw::output output,
                                            dxcw::shader_model sm,
                                            bool build_debug,
                                            cc::span<char const* const> opt_additional_include_paths,
                                            char const* opt_filename_for_errors,
                                            cc::span<char const* const> opt_defines,
                                            cc::allocator* scratch_alloc)
{
#ifdef DXCW_HAS_OPTICK
    OPTICK_EVENT();
#endif
    CC_ASSERT(_lib != nullptr && "Uninitialized dxcw::compiler");

    shader_description shader = {};
    shader.raw_text = raw_text;
    shader.entrypoint = entrypoint;
    shader.target = target;
    shader.sm = sm;

    compilation_config config = {};
    config.output_format = output;
    config.build_debug = build_debug;
    config.additional_include_paths = opt_additional_include_paths;
    config.defines = opt_defines;
    config.filename_for_errors = opt_filename_for_errors;

    IDxcResult* result = this->compile_shader_result(shader, config, scratch_alloc);
    DEFER_RELEASE(result);

    // Print errors and warning if present
    IDxcBlobUtf8* pErrors = nullptr;
    char* pErrorString = nullptr;
    DEFER_RELEASE(pErrors);
    if (get_result_error_string(result, &pErrors, &pErrorString))
    {
        DXCW_LOG_ERROR(R"(shader "{}", entrypoint "{}" ({}):)", opt_filename_for_errors, entrypoint, get_output_type_literal(output));
        DXCW_LOG_ERROR("{}", pErrorString);

        //        DXCW_LOG_ERROR("include root: {}", opt_additional_include_paths);
        //        DXCW_LOG_ERROR("working dir: {}", std::filesystem::current_path().string().c_str());
        //        DXCW_LOG_ERROR("compiling primary source of {} chars", raw_text_length);
    }

    // Return binary blob
    binary res = {};
    if (!get_result_binary(result, &res))
    {
        DXCW_LOG_ERROR("compilation failed");
        return binary{nullptr};
    }
    return res;
}

IDxcResult* dxcw::compiler::compile_shader_result(shader_description const& shader, compilation_config const& config, cc::allocator* scratch_alloc)
{
#ifdef DXCW_HAS_OPTICK
    OPTICK_EVENT();
#endif

    CC_CONTRACT(shader.raw_text);
    CC_CONTRACT(shader.entrypoint);
    CC_ASSERT(_lib != nullptr && "Uninitialized dxcw::compiler");

    IDxcBlobEncoding* encoding = nullptr;
    DEFER_RELEASE(encoding);

    // nocheckin what's the use here?
    auto const raw_text_length = uint32_t(std::strlen(shader.raw_text));
    CC_ASSERT(raw_text_length > 0 && "DXCW shader src text empty");
    _lib->CreateBlobWithEncodingFromPinned(shader.raw_text, raw_text_length, CP_UTF8, &encoding);

    widechar_memory wmem;
    size_t num_chars_defines = 0;
    for (char const* const define : config.defines)
    {
        num_chars_defines += std::strlen(define) + 1;
    }
    wmem.initialize(1024 + 1024 + 1024 + num_chars_defines, scratch_alloc);

    argument_memory argmem;
    argmem.initialize(30, scratch_alloc);

    if (config.filename_for_errors)
    {
        // the filename for errors is simply any non-flag argument to the compilation
        argmem.add_arg(wmem.convert_and_add_text(config.filename_for_errors));
    }

    if (config.output_format == output::spirv)
    {
        // SPIR-V specific flags

        // -fvk-use-dx-layout: no std140/std430/other vulkan-specific layouting, behave just like HLSL->D3D12
        // -fvk-b/t/u/s-shift: shift registers up to avoid overlap, phi-specific
        wchar_t const* spirv_args[] = {
            L"-spirv", L"-fspv-target-env=vulkan1.1", L"-fvk-use-dx-layout", L"-fvk-b-shift", L"0", L"all", L"-fvk-t-shift", L"1000", L"all", L"-fvk-u-shift", L"2000", L"all", L"-fvk-s-shift", L"3000", L"all"};
        argmem.add_multiple_args(spirv_args, CC_COUNTOF(spirv_args));

        if (shader.target == target::vertex || shader.target == target::geometry || shader.target == target::domain)
        {
            // -fvk-invert-y (only in vs/gs/ds): line up vulkans flipped viewport to behave just like HLSL->D3D12
            argmem.add_arg(L"-fvk-invert-y");
        }
    }
    else if (config.output_format == output::dxil)
    {
        // suppress warnings about [[vk::push_constant]] when compiling to dxil
        argmem.add_arg(L"-Wno-ignored-attributes");
    }

    // entrypoint
    argmem.add_arg(L"-E");
    argmem.add_arg(wmem.convert_and_add_text(shader.entrypoint));

    // include paths
    for (char const* additional_include_path : config.additional_include_paths)
    {
        argmem.add_arg(L"-I");
        argmem.add_arg(wmem.convert_and_add_text(additional_include_path));
    }

    if (config.build_debug)
    {
        argmem.add_arg(L"-Od");           // disable optimization
        argmem.add_arg(L"-Zi");           // -Zi: build debug information
        argmem.add_arg(L"-Qembed_debug"); // embed debug info as opposed to creating a PDB
    }
    else
    {
        argmem.add_arg(L"-O3"); // full optimization
    }

    // profile target
    argmem.add_arg(L"-T");
    argmem.add_arg(wmem.add_profile_text(shader.target, shader.sm));

    // defines
    for (char const* const define : config.defines)
    {
        argmem.add_arg(L"-D");
        argmem.add_arg(wmem.convert_and_add_text(define));
    }

    DxcBuffer source_buffer;
    source_buffer.Ptr = shader.raw_text;
    source_buffer.Size = raw_text_length;
    source_buffer.Encoding = CP_UTF8;

    IDxcResult* result = nullptr;
    _compiler->Compile(&source_buffer, argmem.get_data(), argmem.get_num(), _include_handler, IID_PPV_ARGS(&result));

    return result;
}

bool dxcw::compiler::is_result_successful(IDxcResult* result)
{
    HRESULT hrStatus;
    return result && SUCCEEDED(result->GetStatus(&hrStatus)) && SUCCEEDED(hrStatus);
}

bool dxcw::compiler::get_result_binary(IDxcResult* result, binary* out_binary)
{
    if (!is_result_successful(result))
        return false;

    // Return binary blob
    IDxcBlob* pShader = nullptr;
    IDxcBlobUtf16* pOutObjectName = nullptr;
    DEFER_RELEASE(pOutObjectName);
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pShader), &pOutObjectName)))
        return false;

    *out_binary = binary{pShader};
    return true;
}

bool dxcw::compiler::get_result_error_string(IDxcResult* result, IDxcBlobUtf8** out_blob_to_free, char** out_error_string)
{
    CC_CONTRACT(out_blob_to_free && out_error_string);
    if (!result)
        return false;

    IDxcBlobUtf16* pOutErrorName = nullptr;
    DEFER_RELEASE(pOutErrorName);
    if (FAILED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(out_blob_to_free), &pOutErrorName)) || *out_blob_to_free == nullptr)
        return false;

    // Note that d3dcompiler would return null if no errors or warnings are present.
    // IDxcCompiler3::Compile will always return an error buffer, but its length will be zero if there are no warnings or errors.
    if ((*out_blob_to_free)->GetStringLength() == 0)
    {
        (*out_blob_to_free)->Release();
        *out_blob_to_free = nullptr;
        return false;
    }

    *out_error_string = static_cast<char*>((*out_blob_to_free)->GetBufferPointer());
    return true;
}

void dxcw::compiler::free_result_error_blob(IDxcBlobUtf8* blob_to_free)
{
    if (blob_to_free)
        blob_to_free->Release();
}

bool dxcw::compiler::get_result_reflection(IDxcResult* result, D3D12_SHADER_DESC* out_shader_desc)
{
#ifdef CC_OS_WINDOWS
    if (!result)
        return false;

    CC_CONTRACT(out_shader_desc);
    CC_ASSERT(_utils != nullptr && "Called on unitialized compiler");

    IDxcBlob* pReflection = nullptr;
    IDxcBlobUtf16* pOutReflectionName = nullptr;
    DEFER_RELEASE(pReflection);
    DEFER_RELEASE(pOutReflectionName);
    if (FAILED(result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&pReflection), &pOutReflectionName)))
        return false;

    DxcBuffer ReflBuffer = {};
    ReflBuffer.Ptr = pReflection->GetBufferPointer();
    ReflBuffer.Size = pReflection->GetBufferSize();

    ID3D12ShaderReflection* pShaderReflection = nullptr;
    DEFER_RELEASE(pShaderReflection);
    if (FAILED(_utils->CreateReflection(&ReflBuffer, IID_PPV_ARGS(&pShaderReflection))))
        return false;

    D3D12_SHADER_DESC ShaderDesc = {};
    return SUCCEEDED(pShaderReflection->GetDesc(&ShaderDesc));

    // DXCW_LOG_TRACE("Shader stats: %u instructions (%u Float, %u Int, %u uint)", ShaderDesc.InstructionCount, ShaderDesc.FloatInstructionCount,
    //                ShaderDesc.IntInstructionCount, ShaderDesc.UintInstructionCount);
    // DXCW_LOG_TRACE("              %u static branches, %u dynamic branches", ShaderDesc.StaticFlowControlCount, ShaderDesc.DynamicFlowControlCount);
    // DXCW_LOG_TRACE("              %u temp registers, %u temp arrays", ShaderDesc.TempRegisterCount, ShaderDesc.TempArrayCount);

#else
    // D3D12 is not available on linux
    DXCW_LOG_ERROR("Shader reflection is unavailable without D3D12");
    return false;
#endif
}

dxcw::binary dxcw::compiler::compile_library(const char* raw_text,
                                             cc::span<const library_export> exports,
                                             dxcw::output output,
                                             bool build_debug,
                                             cc::span<char const* const> opt_additional_include_paths,
                                             const char* opt_filename_for_errors,
                                             cc::span<char const* const> opt_defines,
                                             cc::allocator* scratch_alloc)
{
#ifdef DXCW_HAS_OPTICK
    OPTICK_EVENT();
#endif

    CC_CONTRACT(raw_text);
    CC_ASSERT(_lib != nullptr && "Uninitialized dxcw::compiler");

    IDxcBlobEncoding* encoding = nullptr;
    DEFER_RELEASE(encoding);

    auto const raw_text_length = uint32_t(std::strlen(raw_text));
    CC_ASSERT(raw_text_length > 0 && "DXCW shader src text empty");
    _lib->CreateBlobWithEncodingFromPinned(raw_text, raw_text_length, CP_UTF8, &encoding);
    widechar_memory wmem;
    size_t num_chars_defines = 0;
    for (char const* const define : opt_defines)
    {
        num_chars_defines += std::strlen(define) + 1;
    }
    wmem.initialize(1024 + 1024 + 1024 + num_chars_defines, scratch_alloc);

    argument_memory argmem;
    argmem.initialize(30 + exports.size() * 2, scratch_alloc);

    if (opt_filename_for_errors)
    {
        // the filename for errors is simply any non-flag argument to the compilation
        argmem.add_arg(wmem.convert_and_add_text(opt_filename_for_errors));
    }

    if (output == output::spirv)
    {
        // SPIR-V specific flags
        // -spirv: output SPIR-V
        // -fspv-target-env=vulkan1.1: target Vk 1.1
        // -fspv-extension=SPV_NV_ray_tracing: use SKV_NV_ray_tracing (as opposed to the default SPV_KHR_ray_tracing)
        // -fvk-use-dx-layout: no std140/std430/other vulkan-specific layouting, behave just like HLSL->D3D12
        // -fvk-b/t/u/s-shift: shift registers up to avoid overlap, phi-specific

        wchar_t const* spirv_args[] = {L"-spirv", L"-fspv-target-env=vulkan1.1",
                                       //                                       L"-fspv-extension=SPV_NV_ray_tracing",
                                       L"-fvk-use-dx-layout", L"-fspv-reflect", L"-fvk-b-shift", L"0", L"all", L"-fvk-t-shift", L"1000", L"all",
                                       L"-fvk-u-shift", L"2000", L"all", L"-fvk-s-shift", L"3000", L"all"};
        argmem.add_multiple_args(spirv_args, CC_COUNTOF(spirv_args));
    }
    else if (output == output::dxil)
    {
        // suppress warnings about [[vk::push_constant]] when compiling to dxil
        argmem.add_arg(L"-Wno-ignored-attributes");
    }

    // profile target
    argmem.add_arg(L"-T");
    argmem.add_arg(L"lib_" DXCW_DEFAULT_SHADER_MODEL_STR);

    // include paths
    for (char const* additional_include_path : opt_additional_include_paths)
    {
        argmem.add_arg(L"-I");
        argmem.add_arg(wmem.convert_and_add_text(additional_include_path));
    }


    if (build_debug)
    {
        argmem.add_arg(L"-Od");           // disable optimization
        argmem.add_arg(L"-Zi");           // -Zi: build debug information
        argmem.add_arg(L"-Qembed_debug"); // embed debug info as opposed to creating a PDB
    }
    else
    {
        argmem.add_arg(L"-O3"); // full optimization
    }

    // defines
    for (char const* const define : opt_defines)
    {
        argmem.add_arg(L"-D");
        argmem.add_arg(wmem.convert_and_add_text(define));
    }

    // exports
    auto export_text = cc::alloc_array<wchar_t>::uninitialized(exports.size() * 128, scratch_alloc);
    unsigned num_chars_export_text = 0;

    [[maybe_unused]] auto const f_add_export_wchars = [&](wchar_t const* text, unsigned strlen)
    {
        CC_ASSERT(num_chars_export_text + strlen <= export_text.size() && "text write OOB");
        std::memcpy(export_text.data() + num_chars_export_text, text, strlen * sizeof(wchar_t));
        num_chars_export_text += strlen;
    };

    auto const f_add_export_entry_text = [&](char const* export_name, char const* internal_name) -> wchar_t const*
    {
        CC_ASSERT(internal_name != nullptr && "internal name is required on library exports");

        // from dxc.exe -help:
        //   -exports <value>        Specify exports when compiling a library: export1[[,export1_clone,...]=internal_name][;...]
        // form of export entries:
        // <export name>=<internal name>, f.e. closest_hit=MainClosestHit
        // or only the export value if it's the same
        // <export name>

        wchar_t const* const res = export_text.data() + num_chars_export_text;

        if (export_name)
        {
            // convert and write the export name

            auto const num_wchars_written
                = cc::char_to_widechar(cc::span{export_text}.subspan(num_chars_export_text, export_text.size() - num_chars_export_text), export_name);
            num_chars_export_text += unsigned(num_wchars_written) + 1;

            // write the equals sign
            export_text[num_chars_export_text] = L'=';
            ++num_chars_export_text;
        }

        // convert and write the internal name
        auto const num_wchars_written
            = cc::char_to_widechar(cc::span{export_text}.subspan(num_chars_export_text, export_text.size() - num_chars_export_text), internal_name);
        num_chars_export_text += unsigned(num_wchars_written) + 1;


        return res;
    };

    for (auto const& exp : exports)
    {
        argmem.add_arg(L"-exports");
        argmem.add_arg(f_add_export_entry_text(exp.export_name, exp.internal_name));
    }

    DxcBuffer source_buffer;
    source_buffer.Ptr = raw_text;
    source_buffer.Size = std::strlen(raw_text);
    source_buffer.Encoding = CP_UTF8;

    IDxcResult* result = nullptr;
    DEFER_RELEASE(result);
    _compiler->Compile(&source_buffer, argmem.get_data(), argmem.get_num(), _include_handler, IID_PPV_ARGS(&result));

    // Print errors and warning if present
    IDxcBlobUtf8* pErrors = nullptr;
    DEFER_RELEASE(pErrors);
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
    // Note that d3dcompiler would return null if no errors or warnings are present.
    // IDxcCompiler3::Compile will always return an error buffer, but its length will be zero if there are no warnings or errors.
    if (pErrors != nullptr && pErrors->GetStringLength() != 0)
    {
        DXCW_LOG_ERROR(R"(shader library "{}", ({}):)", opt_filename_for_errors, get_output_type_literal(output));
        DXCW_LOG_ERROR("{}", static_cast<char const*>(pErrors->GetBufferPointer()));
    }

    // Quit on failure
    HRESULT hrStatus;
    result->GetStatus(&hrStatus);
    if (FAILED(hrStatus))
    {
        DXCW_LOG_ERROR("compilation failed");
        return binary{nullptr};
    }


    // Return binary blob
    IDxcBlob* pShader = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pShader), nullptr);
    return binary{pShader};
}

bool dxcw::compiler::print_version() const
{
    unsigned ver_maj = 0, ver_min = 0, ver_commit_i = 0;
    char const* ver_commit_hash = nullptr;

    if (get_version(ver_maj, ver_min))
    {
        if (get_version_commit(ver_commit_i, ver_commit_hash))
            DXCW_LOG("DXC v{}.{} (dev {}-{})", ver_maj, ver_min, ver_commit_i, ver_commit_hash);
        else
            DXCW_LOG("DXC v{}.{}", ver_maj, ver_min, ver_commit_i, ver_commit_hash);

        return true;
    }
    else
    {
        DXCW_LOG_WARN("failed to query DXC version");
        return false;
    }
}

bool dxcw::compiler::get_version(unsigned& out_major, unsigned& out_minor) const
{
    IDxcVersionInfo* version_info = nullptr;
    auto hres = _compiler->QueryInterface(IID_PPV_ARGS(&version_info));

    if (SUCCEEDED(hres) && version_info)
    {
        hres = version_info->GetVersion(&out_major, &out_minor);
        version_info->Release();
        return SUCCEEDED(hres);
    }

    out_major = 0;
    out_minor = 0;
    return false;
}

bool dxcw::compiler::get_version_commit(unsigned& out_num_commits, const char*& out_commit_hash) const
{
    IDxcVersionInfo2* version_info = nullptr;
    auto hres = _compiler->QueryInterface(IID_PPV_ARGS(&version_info));

    if (SUCCEEDED(hres) && version_info)
    {
        char* commit_hash_com = nullptr;
        hres = version_info->GetCommitInfo(&out_num_commits, &commit_hash_com);

        if (SUCCEEDED(hres) && commit_hash_com)
        {
            out_commit_hash = commit_hash_com;
            return true;
        }
    }

    out_num_commits = 0;
    out_commit_hash = nullptr;
    return false;
}

dxcw::shader_model dxcw::compiler::get_default_shader_model() { return DXCW_DEFAULT_SHADER_MODEL_ENUM; }


dxcw::binary::binary(IDxcBlob* blob)
{
    internal_blob = blob;
    if (blob)
    {
        data = static_cast<std::byte*>(blob->GetBufferPointer());
        size = blob->GetBufferSize();
    }
}

void dxcw::destroy_blob(IDxcBlob* blob)
{
    if (blob != nullptr)
    {
        blob->Release();
    }
}

void dxcw::destroy_result(IDxcResult* blob)
{
    if (blob)
        blob->Release();
}

void dxcw::destroy(const dxcw::binary& b) { destroy_blob(b.internal_blob); }
