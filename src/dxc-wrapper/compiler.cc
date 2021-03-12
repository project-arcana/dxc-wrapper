#include "compiler.hh"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

// clang-format off
//#include <clean-core/native/detail/win32_sanitize_before.inl> // WIN32_LEAN_AND_MEAN kills features we require here

struct IUnknown;
#pragma warning(push, 0)

#include <Windows.h>
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

#define DXCW_DEFAULT_SHADER_MODEL "6_4"

namespace
{
wchar_t const* get_profile_literal(dxcw::target target)
{
    using ct = dxcw::target;
    switch (target)
    {
    case ct::vertex:
        return L"vs_" DXCW_DEFAULT_SHADER_MODEL;
    case ct::hull:
        return L"hs_" DXCW_DEFAULT_SHADER_MODEL;
    case ct::domain:
        return L"ds_" DXCW_DEFAULT_SHADER_MODEL;
    case ct::geometry:
        return L"gs_" DXCW_DEFAULT_SHADER_MODEL;
    case ct::pixel:
        return L"ps_" DXCW_DEFAULT_SHADER_MODEL;
    case ct::compute:
        return L"cs_" DXCW_DEFAULT_SHADER_MODEL;
    case ct::mesh:
        return L"ms_" DXCW_DEFAULT_SHADER_MODEL;
    case ct::amplification:
        return L"as_" DXCW_DEFAULT_SHADER_MODEL;
    default:
        return L"ERR_UNKNOWN_TARGET";
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

    wchar_t const* convert_and_add_text(char const* text)
    {
        auto const destination_span = cc::span(buffer).subspan(num_chars, buffer.size() - num_chars);
        auto const num_wchars_written = cc::char_to_widechar(destination_span, text);
        num_chars += num_wchars_written + 1;

        return destination_span.data();
    }
};
}

void dxcw::compiler::initialize()
{
    CC_ASSERT(_lib == nullptr && "double initialize");
    verify_hres(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&_lib)));
    verify_hres(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&_compiler)));
    verify_hres(_lib->CreateIncludeHandler(&_include_handler));
    // verify_hres(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&_reflection)));
}

void dxcw::compiler::destroy()
{
    if (_lib == nullptr)
        return;

    //_reflection->Release();
    //_reflection = nullptr;
    _include_handler->Release();
    _include_handler = nullptr;
    _compiler->Release();
    _compiler = nullptr;
    _lib->Release();
    _lib = nullptr;
}

dxcw::binary dxcw::compiler::compile_shader(const char* raw_text,
                                            const char* entrypoint,
                                            dxcw::target target,
                                            dxcw::output output,
                                            bool build_debug,
                                            char const* opt_additional_include_paths,
                                            char const* opt_filename_for_errors,
                                            cc::span<char const*> opt_defines,
                                            cc::allocator* scratch_alloc)
{
    CC_CONTRACT(raw_text);
    CC_CONTRACT(entrypoint);
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
    argmem.initialize(30, scratch_alloc);

    if (opt_filename_for_errors)
    {
        // the filename for errors is simply any non-flag argument to the compilation
        argmem.add_arg(wmem.convert_and_add_text(opt_filename_for_errors));
    }

    if (output == output::spirv)
    {
        // SPIR-V specific flags

        // -fvk-use-dx-layout: no std140/std430/other vulkan-specific layouting, behave just like HLSL->D3D12
        // -fvk-b/t/u/s-shift: shift registers up to avoid overlap, phi-specific
        wchar_t const* spirv_args[] = {
            L"-spirv", L"-fspv-target-env=vulkan1.1", L"-fvk-use-dx-layout", L"-fvk-b-shift", L"0", L"all", L"-fvk-t-shift", L"1000", L"all", L"-fvk-u-shift", L"2000", L"all", L"-fvk-s-shift", L"3000", L"all"};
        argmem.add_multiple_args(spirv_args, CC_COUNTOF(spirv_args));

        if (target == target::vertex || target == target::geometry || target == target::domain)
        {
            // -fvk-invert-y (only in vs/gs/ds): line up vulkans flipped viewport to behave just like HLSL->D3D12
            argmem.add_arg(L"-fvk-invert-y");
        }
    }
    else if (output == output::dxil)
    {
        // suppress warnings about [[vk::push_constant]] when compiling to dxil
        argmem.add_arg(L"-Wno-ignored-attributes");
    }

    // entrypoint
    argmem.add_arg(L"-E");
    argmem.add_arg(wmem.convert_and_add_text(entrypoint));

    // include paths
    if (opt_additional_include_paths != nullptr)
    {
        argmem.add_arg(L"-I");
        argmem.add_arg(wmem.convert_and_add_text(opt_additional_include_paths));
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

    // profile target
    argmem.add_arg(L"-T");
    argmem.add_arg(get_profile_literal(target));

    // defines
    for (char const* const define : opt_defines)
    {
        argmem.add_arg(L"-D");
        argmem.add_arg(wmem.convert_and_add_text(define));
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
    IDxcBlobUtf16* pOutputName = nullptr;
    DEFER_RELEASE(pErrors);
    DEFER_RELEASE(pOutputName);
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), &pOutputName);

    // Note that d3dcompiler would return null if no errors or warnings are present.
    // IDxcCompiler3::Compile will always return an error buffer, but its length will be zero if there are no warnings or errors.
    if (pErrors != nullptr && pErrors->GetStringLength() != 0)
    {
        DXCW_LOG_ERROR(R"(shader "{}", entrypoint "{}" ({}):)", opt_filename_for_errors, entrypoint, get_output_type_literal(output));
        DXCW_LOG_ERROR("{}", static_cast<char const*>(pErrors->GetBufferPointer()));

        //        DXCW_LOG_ERROR("include root: {}", opt_additional_include_paths);
        //        DXCW_LOG_ERROR("working dir: {}", std::filesystem::current_path().string().c_str());
        //        DXCW_LOG_ERROR("compiling primary source of {} chars", raw_text_length);
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

dxcw::binary dxcw::compiler::compile_library(const char* raw_text,
                                             cc::span<const library_export> exports,
                                             dxcw::output output,
                                             bool build_debug,
                                             const char* opt_additional_include_paths,
                                             const char* opt_filename_for_errors,
                                             const char* opt_defines,
                                             cc::allocator* scratch_alloc)
{
    CC_CONTRACT(raw_text);
    CC_ASSERT(_lib != nullptr && "Uninitialized dxcw::compiler");

    wchar_t include_path_wide[1024];
    wchar_t opt_filename_wide[1024];

    IDxcBlobEncoding* encoding = nullptr;
    DEFER_RELEASE(encoding);

    auto const raw_text_length = uint32_t(std::strlen(raw_text));
    CC_ASSERT(raw_text_length > 0 && "DXCW shader src text empty");
    _lib->CreateBlobWithEncodingFromPinned(raw_text, raw_text_length, CP_UTF8, &encoding);

    auto compile_argument_ptrs = cc::alloc_array<LPCWSTR>::uninitialized(32 + exports.size() * 2, scratch_alloc);
    unsigned num_compile_arguments = 0;

    auto const f_add_compile_arg = [&](wchar_t const* str) {
        CC_ASSERT(num_compile_arguments < compile_argument_ptrs.size() && "write OOB");
        compile_argument_ptrs[num_compile_arguments] = str;
        ++num_compile_arguments;
    };

    auto const f_add_multiple_compile_args = [&](wchar_t const* const* strs, unsigned num) {
        CC_ASSERT(num_compile_arguments + num <= compile_argument_ptrs.size() && "write OOB");
        std::memcpy(compile_argument_ptrs.data() + num_compile_arguments, &strs[0], num * sizeof(wchar_t const*));
        num_compile_arguments += num;
    };

    if (opt_filename_for_errors)
    {
        cc::char_to_widechar(opt_filename_wide, opt_filename_for_errors);
        // the filename for errors is simply any non-flag argument to the compilation
        f_add_compile_arg(opt_filename_wide);
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
        f_add_multiple_compile_args(spirv_args, CC_COUNTOF(spirv_args));
    }
    else if (output == output::dxil)
    {
        // suppress warnings about [[vk::push_constant]] when compiling to dxil
        f_add_compile_arg(L"-Wno-ignored-attributes");
    }

    f_add_compile_arg(L"-T");
    f_add_compile_arg(L"lib_" DXCW_DEFAULT_SHADER_MODEL);

    // include paths
    if (opt_additional_include_paths != nullptr)
    {
        cc::char_to_widechar(include_path_wide, opt_additional_include_paths);

        f_add_compile_arg(L"-I");
        f_add_compile_arg(include_path_wide);
    }


    if (build_debug)
    {
        f_add_compile_arg(L"-Od");           // disable optimization
        f_add_compile_arg(L"-Zi");           // -Zi: build debug information
        f_add_compile_arg(L"-Qembed_debug"); // embed debug info as opposed to creating a PDB
    }
    else
    {
        f_add_compile_arg(L"-O3"); // full optimization
    }

    // defines
    cc::alloc_array<wchar_t> define_text;
    if (opt_defines != nullptr)
    {
        define_text = cc::alloc_array<wchar_t>::uninitialized(std::strlen(opt_defines), scratch_alloc);
        cc::char_to_widechar(define_text, opt_defines);

        f_add_compile_arg(L"-D");
        f_add_compile_arg(define_text.data());
    }

    // exports
    auto export_text = cc::alloc_array<wchar_t>::uninitialized(exports.size() * 128, scratch_alloc);
    unsigned num_chars_export_text = 0;

    [[maybe_unused]] auto const f_add_export_wchars = [&](wchar_t const* text, unsigned strlen) {
        CC_ASSERT(num_chars_export_text + strlen <= export_text.size() && "text write OOB");
        std::memcpy(export_text.data() + num_chars_export_text, text, strlen * sizeof(wchar_t));
        num_chars_export_text += strlen;
    };

    auto const f_add_export_entry_text = [&](char const* export_name, char const* internal_name) -> wchar_t const* {
        CC_ASSERT(internal_name != nullptr && "internal name is required on library exports");
        // form of export entries:
        // <exported name>=<internal name>, f.e. closest_hit=MainClosestHit

        wchar_t const* const res = export_text.data() + num_chars_export_text;

        // write the target
        //  unsigned target_strlen;
        // auto const target_as_wchar = get_library_export_name(tgt, target_strlen);
        //  f_add_export_wchars(target_as_wchar, target_strlen);

        // convert and write the export name
        {
            auto const num_wchars_written = cc::char_to_widechar(
                cc::span{export_text}.subspan(num_chars_export_text, export_text.size() - num_chars_export_text), export_name ? export_name : internal_name);
            num_chars_export_text += unsigned(num_wchars_written) + 1;
        }

        // write the equals sign
        export_text[num_chars_export_text] = L'=';
        ++num_chars_export_text;

        // convert and write the internal name
        {
            auto const num_wchars_written
                = cc::char_to_widechar(cc::span{export_text}.subspan(num_chars_export_text, export_text.size() - num_chars_export_text), internal_name);
            num_chars_export_text += unsigned(num_wchars_written) + 1;
        }

        return res;
    };

    for (auto const& exp : exports)
    {
        f_add_compile_arg(L"-exports");
        f_add_compile_arg(f_add_export_entry_text(exp.export_name, exp.internal_name));
    }

    DxcBuffer source_buffer;
    source_buffer.Ptr = raw_text;
    source_buffer.Size = std::strlen(raw_text);
    source_buffer.Encoding = CP_UTF8;

    IDxcResult* result = nullptr;
    DEFER_RELEASE(result);

    _compiler->Compile(&source_buffer, compile_argument_ptrs.data(), num_compile_arguments, _include_handler, IID_PPV_ARGS(&result));

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

    //    IDxcBlobUtf8* pReflection = nullptr;
    //    DEFER_RELEASE(pReflection);
    //    result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&pReflection), nullptr);
    //    if (pReflection != nullptr && pReflection->GetStringLength() != 0)
    //    {
    //        DXCW_LOG("{}", static_cast<char const*>(pReflection->GetBufferPointer()));
    //    }
    //    else
    //    {
    //        DXCW_LOG_ERROR("failed to receive reflection");
    //    }

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

void dxcw::destroy(const dxcw::binary& b) { destroy_blob(b.internal_blob); }
