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
                                            wchar_t const* opt_binary_name,
                                            wchar_t const* opt_additional_include_paths,
                                            bool build_debug_info,
                                            const char* opt_filename_for_errors)
{
    CC_ASSERT(_lib != nullptr && "Uninitialized dxcw::compiler");

    IDxcBlobEncoding* encoding;
    _lib->CreateBlobWithEncodingFromPinned(raw_text, static_cast<uint32_t>(std::strlen(raw_text)), CP_UTF8, &encoding);
    CC_DEFER { encoding->Release(); };

    cc::capped_vector<LPCWSTR, 20> compile_flags;

    if (output == output::spirv)
    {
        // -fvk-use-dx-layout: no std140/std430/other vulkan-specific layouting, behave just like HLSL->D3D12
        // -fvk-b/t/u/s-shift: shift registers up to avoid overlap, phi-specific
        compile_flags = {
            L"-spirv", L"-fspv-target-env=vulkan1.1", L"-fvk-use-dx-layout", L"-fvk-b-shift", L"0", L"all", L"-fvk-t-shift", L"1000", L"all", L"-fvk-u-shift", L"2000", L"all", L"-fvk-s-shift", L"3000", L"all"};

        if (target == target::vertex || target == target::geometry || target == target::domain)
        {
            // -fvk-invert-y (only in vs/gs/ds): line up vulkans flipped viewport to behave just like HLSL->D3D12
            compile_flags.push_back(L"-fvk-invert-y");
        }
    }

    if (opt_additional_include_paths != nullptr)
    {
        compile_flags.push_back(L"/I");
        compile_flags.push_back(opt_additional_include_paths);
    }

    if (build_debug_info)
        compile_flags.push_back(L"/Zi");

    wchar_t entrypoint_wide[64];
    {
        // wchar must die
        std::mbstate_t state = std::mbstate_t();
        char const* entrypoint_copy = entrypoint; // copy the pointer as mbsrtowcs writes to it ('entrypoint' is used later on in log output)
        std::mbsrtowcs(entrypoint_wide, &entrypoint_copy, 64, &state);
    }

    IDxcOperationResult* compile_result;
    _compiler->Compile(encoding,                                                       // program text
                       opt_binary_name != nullptr ? opt_binary_name : L"unknown.hlsl", // file name, mostly for error messages
                       entrypoint_wide,                                                // entry point function
                       get_profile_literal(target),                                    // target profile
                       compile_flags.empty() ? nullptr : compile_flags.data(),         // compilation arguments
                       static_cast<uint32_t>(compile_flags.size()),                    // number of compilation arguments
                       nullptr, 0,                                                     // name/value defines and their count
                       _include_handler,                                               // handler for #include directives
                       &compile_result);
    CC_DEFER { compile_result->Release(); };

    HRESULT hres;
    compile_result->GetStatus(&hres);

    if (SUCCEEDED(hres))
    {
        IDxcBlob* result_blob;
        compile_result->GetResult(&result_blob);
        return binary{result_blob};
    }
    else
    {
        IDxcBlobEncoding* error_buffer;
        compile_result->GetErrorBuffer(&error_buffer);

        if (opt_filename_for_errors)
        {
            std::fprintf(stderr, "[dxc-wrapper] error compiling shader \"%s\", entrypoint \"%s\":\n%s\n", opt_filename_for_errors, entrypoint,
                         static_cast<char const*>(error_buffer->GetBufferPointer()));
        }
        else
        {
            std::fprintf(stderr, "[dxc-wrapper] error compiling shader:\n%s\n", static_cast<char const*>(error_buffer->GetBufferPointer()));
        }

        error_buffer->Release();
        return binary{nullptr};
    }
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
