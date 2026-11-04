#if defined(FD_NATIVE_RENDERER) && defined(FD_XENIA_GPU)
#include "shader_capabilities.h"
#include "shader_catalog.h"
#include "xenia/gpu/shader.h"
#include "xenia/base/string_buffer.h"
#include <cstdio>
#include <stdexcept>

namespace gpu::native
{
namespace
{
uint32_t Big(const std::vector<uint8_t>& bytes, size_t at)
{
    if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("Truncated shader capability metadata");
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at+1]) << 16 | uint32_t(bytes[at+2]) << 8 | bytes[at+3];
}
}
int SurveyShaderCapabilities(const std::string& path, const std::string& report)
{
    try
    {
        const auto catalog = ShaderCatalog::Load(path);
        struct Result { std::string name; ShaderStage stage; uint32_t mask; std::vector<uint32_t> constants, floats; };
        std::vector<Result> results;
        uint32_t memoryShaders = 0;
        xe::StringBuffer disassembly;
        for (const auto& entry : catalog.Shaders())
        {
            const uint32_t header = Big(entry.virtualData, 24);
            const uint32_t offset = Big(entry.virtualData, header), size = Big(entry.virtualData, header+4);
            if (!size || size % 4 || offset > entry.physicalData.size() || size > entry.physicalData.size() - offset)
                throw std::runtime_error("Invalid shader code range for capability survey");
            const auto type = entry.stage == ShaderStage::Vertex ? xe::gpu::xenos::ShaderType::kVertex : xe::gpu::xenos::ShaderType::kPixel;
            xe::gpu::Shader shader(type, 0, reinterpret_cast<const uint32_t*>(entry.physicalData.data() + offset), size / 4);
            disassembly.Reset();
            shader.AnalyzeUcode(disassembly);
            if (!shader.is_ucode_analyzed()) throw std::runtime_error("Shader capability analysis did not complete");
            const auto& constants = shader.memexport_stream_constants();
            Result result{entry.name, entry.stage, shader.memexport_eM_written(), {constants.begin(),constants.end()}, {}};
            const auto& used = shader.constant_register_map();
            for (uint32_t i=0; i<256; ++i) if (used.float_bitmap[i/64] >> (i%64) & 1) result.floats.push_back(i);
            results.push_back(std::move(result));
            if (shader.memexport_eM_written()) ++memoryShaders;
        }
        FILE* file = fopen(report.c_str(), "wb");
        if (!file) throw std::runtime_error("Cannot create shader capability report");
        fprintf(file,"{\n  \"analyzed_shaders\": %zu,\n  \"memexport_shaders\": %u,\n  \"shaders\": [\n",results.size(),memoryShaders);
        const char* separator = "";
        for (const auto& result : results)
        {
            fprintf(file,"%s    {\"name\": \"%s\", \"stage\": \"%s\", \"memexport_mask\": %u, \"stream_constants\": [",
                separator,result.name.c_str(),result.stage==ShaderStage::Vertex?"vs":"ps",result.mask);
            const char* comma="";
            for(uint32_t constant:result.constants) { fprintf(file,"%s%u",comma,constant); comma=", "; }
            fprintf(file,"], \"float_registers\": ["); comma="";
            for(uint32_t constant:result.floats) { fprintf(file,"%s%u",comma,constant); comma=", "; }
            fprintf(file,"]}"); separator=",\n";
        }
        fprintf(file,"\n  ]\n}\n");
        const bool okay = ferror(file)==0;
        const int closed=fclose(file);
        if(!okay || closed) throw std::runtime_error("Cannot finish shader capability report");
        printf("Shader capability survey: %zu analyzed, %u use memory export\n",results.size(),memoryShaders);
        return 0;
    }
    catch(const std::exception& error) { fprintf(stderr,"shader capability survey: %s\n",error.what()); return 1; }
}
}
#endif
