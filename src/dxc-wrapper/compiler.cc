#include "compiler.hh"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS
#include <Windows.h>
#endif

#include <dxc/dxcapi.h>

#include <clean-core/assert.hh>
#include <clean-core/capped_vector.hh>
#include <clean-core/defer.hh>

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
    }
    return L"ERR_UNKNOWN_TARGET";
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
                                            char const* opt_additional_include_paths,
                                            bool build_debug_info,
                                            const char* opt_filename_for_errors)
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

    cc::capped_vector<LPCWSTR, 24> compile_arguments;

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
    std::mbstate_t state = {};
    char const* entrypoint_copy = entrypoint; // copy the pointer as mbsrtowcs writes to it
    std::mbsrtowcs(entrypoint_wide, &entrypoint_copy, sizeof(entrypoint_wide), &state);

    compile_arguments.push_back(L"-E");
    compile_arguments.push_back(entrypoint_wide);

    // include paths
    if (opt_additional_include_paths != nullptr)
    {
        state = {};
        char const* includepath_copy = opt_additional_include_paths;
        std::mbsrtowcs(include_path_wide, &includepath_copy, sizeof(include_path_wide), &state);

        compile_arguments.push_back(L"-I");
        compile_arguments.push_back(include_path_wide);
    }

    if (build_debug_info)
        compile_arguments.push_back(L"-Zi");

    // profile target
    compile_arguments.push_back(L"-T");
    compile_arguments.push_back(get_profile_literal(target));

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
