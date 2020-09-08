#include "compiler.hh"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS
#include <Windows.h>
#endif

#include <dxc/dxcapi.h>

#include <clean-core/alloc_array.hh>
#include <clean-core/array.hh>
#include <clean-core/assert.hh>
#include <clean-core/capped_vector.hh>
#include <clean-core/defer.hh>
#include <clean-core/native/wchar_conversion.hh>

#include "common/log.hh"

namespace
{
wchar_t const* get_profile_literal(dxcw::target target)
{
    using ct = dxcw::target;
    switch (target)
    {
    case ct::vertex:
        return L"vs_6_3";
    case ct::hull:
        return L"hs_6_3";
    case ct::domain:
        return L"ds_6_3";
    case ct::geometry:
        return L"gs_6_3";
    case ct::pixel:
        return L"ps_6_3";
    case ct::compute:
        return L"cs_6_3";
    case ct::mesh:
        return L"ms_6_3";
    case ct::amplification:
        return L"as_6_3";
    default:
        return L"ERR_UNKNOWN_TARGET";
    }
}

wchar_t const* get_library_export_name(dxcw::target tgt, unsigned& out_strlen)
{
    using ct = dxcw::target;
#define DXCW_CASE_RETURN(_val_)         \
    case ct::_val_:                     \
        out_strlen = sizeof #_val_ - 1; \
        return L#_val_
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
}

void dxcw::compiler::initialize()
{
    CC_ASSERT(_lib == nullptr && "double initialize");
    verify_hres(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&_lib)));
    verify_hres(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&_compiler)));
    verify_hres(_lib->CreateIncludeHandler(&_include_handler));
}

void dxcw::compiler::destroy()
{
    if (_lib == nullptr)
        return;

    _include_handler->Release();
    _include_handler = nullptr;
    _compiler->Release();
    _compiler = nullptr;
    _lib->Release();
    _lib = nullptr;
}

dxcw::binary dxcw::compiler::compile_binary(const char* raw_text,
                                            const char* entrypoint,
                                            dxcw::target target,
                                            dxcw::output output,
                                            bool build_debug_info,
                                            char const* opt_additional_include_paths,
                                            char const* opt_filename_for_errors,
                                            char const* opt_defines,
                                            cc::allocator* scratch_alloc)
{
#define DEFER_RELEASE(_ptr_)  \
    CC_DEFER                  \
    {                         \
        if (_ptr_)            \
            _ptr_->Release(); \
    }

    CC_ASSERT(_lib != nullptr && "Uninitialized dxcw::compiler");

    wchar_t include_path_wide[1024];
    wchar_t entrypoint_wide[64];

    IDxcBlobEncoding* encoding = nullptr;
    DEFER_RELEASE(encoding);
    _lib->CreateBlobWithEncodingFromPinned(raw_text, static_cast<uint32_t>(std::strlen(raw_text)), CP_UTF8, &encoding);

    cc::capped_vector<LPCWSTR, 26> compile_arguments;

    if (output == output::spirv)
    {
        // SPIR-V specific flags

        // -fvk-use-dx-layout: no std140/std430/other vulkan-specific layouting, behave just like HLSL->D3D12
        // -fvk-b/t/u/s-shift: shift registers up to avoid overlap, phi-specific
        compile_arguments = {
            L"-spirv", L"-fspv-target-env=vulkan1.1", L"-fvk-use-dx-layout", L"-fvk-b-shift", L"0", L"all", L"-fvk-t-shift", L"1000", L"all", L"-fvk-u-shift", L"2000", L"all", L"-fvk-s-shift", L"3000", L"all"};

        if (target == target::vertex || target == target::geometry || target == target::domain)
        {
            // -fvk-invert-y (only in vs/gs/ds): line up vulkans flipped viewport to behave just like HLSL->D3D12
            compile_arguments.push_back(L"-fvk-invert-y");
        }
    }
    else if (output == output::dxil)
    {
        // suppress warnings about [[vk::push_constant]] when compiling to dxil
        compile_arguments.push_back(L"-Wno-ignored-attributes");
    }

    // entrypoint
    cc::char_to_widechar(entrypoint_wide, entrypoint);

    compile_arguments.push_back(L"-E");
    compile_arguments.push_back(entrypoint_wide);

    // include paths
    if (opt_additional_include_paths != nullptr)
    {
        cc::char_to_widechar(include_path_wide, opt_additional_include_paths);

        compile_arguments.push_back(L"-I");
        compile_arguments.push_back(include_path_wide);
    }

    if (build_debug_info)
        compile_arguments.push_back(L"-Zi");

    // profile target
    compile_arguments.push_back(L"-T");
    compile_arguments.push_back(get_profile_literal(target));

    // defines
    cc::alloc_array<wchar_t> define_text;
    if (opt_defines != nullptr)
    {
        define_text = cc::alloc_array<wchar_t>::uninitialized(std::strlen(opt_defines), scratch_alloc);
        cc::char_to_widechar(define_text, opt_defines);

        compile_arguments.push_back(L"-D");
        compile_arguments.push_back(define_text.data());
    }

    DxcBuffer source_buffer;
    source_buffer.Ptr = raw_text;
    source_buffer.Size = std::strlen(raw_text);
    source_buffer.Encoding = CP_UTF8;

    IDxcResult* result = nullptr;
    DEFER_RELEASE(result);
    _compiler->Compile(&source_buffer, compile_arguments.empty() ? nullptr : compile_arguments.data(), uint32_t(compile_arguments.size()),
                       _include_handler, IID_PPV_ARGS(&result));


    // Print errors and warning if present
    IDxcBlobUtf8* pErrors = nullptr;
    DEFER_RELEASE(pErrors);
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
    // Note that d3dcompiler would return null if no errors or warnings are present.
    // IDxcCompiler3::Compile will always return an error buffer, but its length will be zero if there are no warnings or errors.
    if (pErrors != nullptr && pErrors->GetStringLength() != 0)
    {
        DXCW_LOG_ERROR("shader \"{}\", entrypoint \"{}\":", opt_filename_for_errors, entrypoint);
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

    // Save pdb
    //    IDxcBlob* pPDB = nullptr;
    //    IDxcBlobUtf16* pPDBName = nullptr;
    //    result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pPDB), &pPDBName);
    //    {
    //        FILE* fp = NULL;

    //        // Note that if you don't specify -Fd, a pdb name will be automatically generated. Use this file name to save the pdb so that PIX can
    //        find it quickly. _wfopen_s(&fp, pPDBName->GetStringPointer(), L"wb"); fwrite(pPDB->GetBufferPointer(), pPDB->GetBufferSize(), 1, fp);
    //        fclose(fp);
    //    }
}

dxcw::binary dxcw::compiler::compile_library(const char* raw_text,
                                             cc::span<const library_export> exports,
                                             dxcw::output output,
                                             bool build_debug_info,
                                             const char* opt_additional_include_paths,
                                             const char* opt_filename_for_errors,
                                             const char* opt_defines,
                                             cc::allocator* scratch_alloc)
{
#define DEFER_RELEASE(_ptr_)  \
    CC_DEFER                  \
    {                         \
        if (_ptr_)            \
            _ptr_->Release(); \
    }

    CC_ASSERT(_lib != nullptr && "Uninitialized dxcw::compiler");

    wchar_t include_path_wide[1024];

    IDxcBlobEncoding* encoding = nullptr;
    DEFER_RELEASE(encoding);
    _lib->CreateBlobWithEncodingFromPinned(raw_text, static_cast<uint32_t>(std::strlen(raw_text)), CP_UTF8, &encoding);

    cc::alloc_array<LPCWSTR> compile_argument_ptrs = cc::alloc_array<LPCWSTR>::uninitialized(26 + exports.size() * 2, scratch_alloc);
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

    if (output == output::spirv)
    {
        // SPIR-V specific flags

        // -fvk-use-dx-layout: no std140/std430/other vulkan-specific layouting, behave just like HLSL->D3D12
        // -fvk-b/t/u/s-shift: shift registers up to avoid overlap, phi-specific

        wchar_t const* spirv_args[] = {
            L"-spirv", L"-fspv-target-env=vulkan1.1", L"-fvk-use-dx-layout", L"-fvk-b-shift", L"0", L"all", L"-fvk-t-shift", L"1000", L"all", L"-fvk-u-shift", L"2000", L"all", L"-fvk-s-shift", L"3000", L"all"};
        f_add_multiple_compile_args(spirv_args, sizeof(spirv_args) / sizeof(spirv_args[0]));
    }
    else if (output == output::dxil)
    {
        // suppress warnings about [[vk::push_constant]] when compiling to dxil
        f_add_compile_arg(L"-Wno-ignored-attributes");
    }

    f_add_compile_arg(L"-T");
    f_add_compile_arg(L"lib_6_3");

    // include paths
    if (opt_additional_include_paths != nullptr)
    {
        cc::char_to_widechar(include_path_wide, opt_additional_include_paths);

        f_add_compile_arg(L"-I");
        f_add_compile_arg(include_path_wide);
    }

    if (build_debug_info)
        f_add_compile_arg(L"-Zi");

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
    auto export_text = cc::alloc_array<wchar_t>::uninitialized(exports.size() * 128);
    unsigned num_chars_export_text = 0;

    auto const f_add_export_wchars = [&](wchar_t const* text, unsigned strlen) {
        CC_ASSERT(num_chars_export_text + strlen <= export_text.size() && "text write OOB");
        std::memcpy(export_text.data() + num_chars_export_text, text, strlen * sizeof(wchar_t));
        num_chars_export_text += strlen;
    };

    auto const f_add_export_entry_text = [&](target tgt, char const* entrypoint) -> wchar_t const* {
        // form of export entries:
        // <target>=<entrypoint>, f.e. closesthit=MainClosestHit

        wchar_t const* const res = export_text.data() + num_chars_export_text;

        // write the target
        unsigned target_strlen;
        auto const target_as_wchar = get_library_export_name(tgt, target_strlen);
        f_add_export_wchars(target_as_wchar, target_strlen);

        // write the equals sign
        export_text[num_chars_export_text] = L'=';
        ++num_chars_export_text;

        // convert and write the entrypoint
        auto const num_wchars_written
            = cc::char_to_widechar(cc::span{export_text}.subspan(num_chars_export_text, export_text.size() - num_chars_export_text), entrypoint);
        num_chars_export_text += num_wchars_written + 1;

        return res;
    };

    for (auto const& exp : exports)
    {
        f_add_compile_arg(L"-exports");
        f_add_compile_arg(f_add_export_entry_text(exp.type, exp.entrypoint));
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
        DXCW_LOG_ERROR("shader library \"{}\":", opt_filename_for_errors);
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

    // Save pdb
    //    IDxcBlob* pPDB = nullptr;
    //    IDxcBlobUtf16* pPDBName = nullptr;
    //    result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pPDB), &pPDBName);
    //    {
    //        FILE* fp = NULL;

    //        // Note that if you don't specify -Fd, a pdb name will be automatically generated. Use this file name to save the pdb so that PIX can
    //        find it quickly. _wfopen_s(&fp, pPDBName->GetStringPointer(), L"wb"); fwrite(pPDB->GetBufferPointer(), pPDB->GetBufferSize(), 1, fp);
    //        fclose(fp);
    //    }
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
