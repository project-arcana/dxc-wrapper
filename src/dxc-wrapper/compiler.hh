#pragma once

#include <cstddef>
#include <cstdint>

struct IDxcBlob;
struct IDxcLibrary;
struct IDxcCompiler;
struct IDxcIncludeHandler;

namespace phi::sc
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

struct compiler
{
public:
    void initialize();
    void destroy();

    [[nodiscard]] binary compile_binary(char const* raw_text,
                                        char const* entrypoint,
                                        target target,
                                        output output,
                                        const wchar_t* binary_name = nullptr,
                                        wchar_t const* additional_include_paths = nullptr,
                                        bool build_debug_info = false);

private:
    IDxcLibrary* _lib = nullptr;
    IDxcCompiler* _compiler = nullptr;
    IDxcIncludeHandler* _include_handler = nullptr;
};

void destroy_blob(IDxcBlob* blob);
}
