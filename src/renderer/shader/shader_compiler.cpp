#include "shader_compiler.hpp"
#include "shader_manager.hpp"
#include "../rhi/vk/ohao_vk_device.hpp"
#include "../../ui/components/console_widget.hpp"
#include <fstream>
#include <sstream>
#include <regex>

// Include glslang headers (you may need to adjust paths based on your setup)
#ifdef OHAO_USE_GLSLANG
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/Include/ResourceLimits.h>
#include <glslang/MachineIndependent/iomapper.h>
#endif

// Include SPIRV-Cross for reflection
#ifdef OHAO_USE_SPIRV_CROSS
#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#endif

namespace ohao {

// Glslang resource limits (can be customized based on your target devices)
#ifdef OHAO_USE_GLSLANG
static const TBuiltInResource DefaultTBuiltInResource = {
    /* .MaxLights = */ 32,
    /* .MaxClipPlanes = */ 6,
    /* .MaxTextureUnits = */ 32,
    /* .MaxTextureCoords = */ 32,
    /* .MaxVertexAttribs = */ 64,
    /* .MaxVertexUniformComponents = */ 4096,
    /* .MaxVaryingFloats = */ 64,
    /* .MaxVertexTextureImageUnits = */ 32,
    /* .MaxCombinedTextureImageUnits = */ 80,
    /* .MaxTextureImageUnits = */ 32,
    /* .MaxFragmentUniformComponents = */ 4096,
    /* .MaxDrawBuffers = */ 32,
    /* .MaxVertexUniformVectors = */ 128,
    /* .MaxVaryingVectors = */ 8,
    /* .MaxFragmentUniformVectors = */ 16,
    /* .MaxVertexOutputVectors = */ 16,
    /* .MaxFragmentInputVectors = */ 15,
    /* .MinProgramTexelOffset = */ -8,
    /* .MaxProgramTexelOffset = */ 7,
    /* .MaxClipDistances = */ 8,
    /* .MaxComputeWorkGroupCountX = */ 65535,
    /* .MaxComputeWorkGroupCountY = */ 65535,
    /* .MaxComputeWorkGroupCountZ = */ 65535,
    /* .MaxComputeWorkGroupSizeX = */ 1024,
    /* .MaxComputeWorkGroupSizeY = */ 1024,
    /* .MaxComputeWorkGroupSizeZ = */ 64,
    /* .MaxComputeUniformComponents = */ 1024,
    /* .MaxComputeTextureImageUnits = */ 16,
    /* .MaxComputeImageUniforms = */ 8,
    /* .MaxComputeAtomicCounters = */ 8,
    /* .MaxComputeAtomicCounterBuffers = */ 1,
    /* .MaxVaryingComponents = */ 60,
    /* .MaxVertexOutputComponents = */ 64,
    /* .MaxGeometryInputComponents = */ 64,
    /* .MaxGeometryOutputComponents = */ 128,
    /* .MaxFragmentInputComponents = */ 128,
    /* .MaxImageUnits = */ 8,
    /* .MaxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .MaxCombinedShaderOutputResources = */ 8,
    /* .MaxImageSamples = */ 0,
    /* .MaxVertexImageUniforms = */ 0,
    /* .MaxTessControlImageUniforms = */ 0,
    /* .MaxTessEvaluationImageUniforms = */ 0,
    /* .MaxGeometryImageUniforms = */ 0,
    /* .MaxFragmentImageUniforms = */ 8,
    /* .MaxCombinedImageUniforms = */ 8,
    /* .MaxGeometryTextureImageUnits = */ 16,
    /* .MaxGeometryOutputVertices = */ 256,
    /* .MaxGeometryTotalOutputComponents = */ 1024,
    /* .MaxGeometryUniformComponents = */ 1024,
    /* .MaxGeometryVaryingComponents = */ 64,
    /* .MaxTessControlInputComponents = */ 128,
    /* .MaxTessControlOutputComponents = */ 128,
    /* .MaxTessControlTextureImageUnits = */ 16,
    /* .MaxTessControlUniformComponents = */ 1024,
    /* .MaxTessControlTotalOutputComponents = */ 4096,
    /* .MaxTessEvaluationInputComponents = */ 128,
    /* .MaxTessEvaluationOutputComponents = */ 128,
    /* .MaxTessEvaluationTextureImageUnits = */ 16,
    /* .MaxTessEvaluationUniformComponents = */ 1024,
    /* .MaxTessPatchComponents = */ 120,
    /* .MaxPatchVertices = */ 32,
    /* .MaxTessGenLevel = */ 64,
    /* .MaxViewports = */ 16,
    /* .MaxVertexAtomicCounters = */ 0,
    /* .MaxTessControlAtomicCounters = */ 0,
    /* .MaxTessEvaluationAtomicCounters = */ 0,
    /* .MaxGeometryAtomicCounters = */ 0,
    /* .MaxFragmentAtomicCounters = */ 8,
    /* .MaxCombinedAtomicCounters = */ 8,
    /* .MaxAtomicCounterBindings = */ 1,
    /* .MaxVertexAtomicCounterBuffers = */ 0,
    /* .MaxTessControlAtomicCounterBuffers = */ 0,
    /* .MaxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .MaxGeometryAtomicCounterBuffers = */ 0,
    /* .MaxFragmentAtomicCounterBuffers = */ 1,
    /* .MaxCombinedAtomicCounterBuffers = */ 1,
    /* .MaxAtomicCounterBufferSize = */ 16384,
    /* .MaxTransformFeedbackBuffers = */ 4,
    /* .MaxTransformFeedbackInterleavedComponents = */ 64,
    /* .MaxCullDistances = */ 8,
    /* .MaxCombinedClipAndCullDistances = */ 8,
    /* .MaxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .limits = */ {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    }
};
#endif

ShaderCompiler::~ShaderCompiler() {
    cleanup();
}

bool ShaderCompiler::initialize() {
    if (initialized) {
        return true;
    }
    
#ifdef OHAO_USE_GLSLANG
    // Initialize glslang
    if (!glslang::InitializeProcess()) {
        OHAO_LOG_ERROR("Failed to initialize glslang process");
        return false;
    }
    
    initialized = true;
    OHAO_LOG("Shader compiler initialized with glslang support");
    return true;
#else
    OHAO_LOG_WARNING("Shader compiler initialized without glslang support (compile-time disabled)");
    initialized = true;
    return true;
#endif
}

void ShaderCompiler::cleanup() {
    if (!initialized) {
        return;
    }
    
#ifdef OHAO_USE_GLSLANG
    glslang::FinalizeProcess();
#endif
    
    initialized = false;
    OHAO_LOG("Shader compiler cleaned up");
}

ShaderCompilationResult ShaderCompiler::compileGLSL(const std::string& source,
                                                    ShaderStage stage,
                                                    const std::string& filename,
                                                    const std::vector<std::string>& defines,
                                                    const std::vector<std::string>& includePaths) {
    ShaderCompilationResult result;
    
    if (!initialized) {
        result.errorMessage = "Shader compiler not initialized";
        return result;
    }
    
#ifdef OHAO_USE_GLSLANG
    // Convert stage to glslang stage
    EShLanguage glslangStage;
    switch (stage) {
        case ShaderStage::VERTEX: glslangStage = EShLangVertex; break;
        case ShaderStage::FRAGMENT: glslangStage = EShLangFragment; break;
        case ShaderStage::GEOMETRY: glslangStage = EShLangGeometry; break;
        case ShaderStage::TESSELLATION_CONTROL: glslangStage = EShLangTessControl; break;
        case ShaderStage::TESSELLATION_EVALUATION: glslangStage = EShLangTessEvaluation; break;
        case ShaderStage::COMPUTE: glslangStage = EShLangCompute; break;
        default:
            result.errorMessage = "Unsupported shader stage";
            return result;
    }
    
    // Preprocess source with defines
    std::vector<std::string> allDefines = defines;
    addBuiltinDefines(allDefines, stage);
    std::string processedSource = preprocessSource(source, allDefines, includePaths);
    
    // Create shader object
    glslang::TShader shader(glslangStage);
    
    // Set source
    const char* sourcePtr = processedSource.c_str();
    const char* filenamePtr = filename.c_str();
    shader.setStringsWithLengthsAndNames(&sourcePtr, nullptr, &filenamePtr, 1);
    
    // Set environment
    shader.setEnvInput(glslang::EShSourceGlsl, glslangStage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    
    // Set auto-mapping for bindings and locations
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    
    // Parse
    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (generateDebugInfo) {
        messages = static_cast<EShMessages>(messages | EShMsgDebugInfo);
    }
    
    if (!shader.parse(&DefaultTBuiltInResource, 100, false, messages)) {
        result.errorMessage = shader.getInfoLog();
        return result;
    }
    
    // Create program and link
    glslang::TProgram program;
    program.addShader(&shader);
    
    if (!program.link(messages)) {
        result.errorMessage = program.getInfoLog();
        return result;
    }
    
    // Generate SPIR-V
    glslang::TIntermediate* intermediate = program.getIntermediate(glslangStage);
    if (!intermediate) {
        result.errorMessage = "Failed to get intermediate representation";
        return result;
    }
    
    glslang::SpvOptions spvOptions;
    spvOptions.generateDebugInfo = generateDebugInfo;
    spvOptions.disableOptimizer = (optimizationLevel == 0);
    spvOptions.optimizeSize = (optimizationLevel == 1);
    
    std::vector<uint32_t> spirv;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*intermediate, spirv, &logger, &spvOptions);
    
    // Check for warnings/errors
    std::string logMessages = logger.getAllMessages();
    if (!logMessages.empty()) {
        if (warningsAsErrors) {
            result.errorMessage = logMessages;
            return result;
        } else {
            result.warningMessage = logMessages;
        }
    }
    
    result.spirvCode = std::move(spirv);
    result.success = true;
    
    OHAO_LOG("Successfully compiled GLSL shader: " + filename);
    return result;
    
#else
    // Fallback implementation without glslang
    result.errorMessage = "GLSL compilation not supported (glslang not available)";
    OHAO_LOG_ERROR("Attempted to compile GLSL without glslang support: " + filename);
    return result;
#endif
}

ShaderCompilationResult ShaderCompiler::compileHLSL(const std::string& source,
                                                    ShaderStage stage,
                                                    const std::string& entryPoint,
                                                    const std::string& filename) {
    ShaderCompilationResult result;
    result.errorMessage = "HLSL compilation not yet implemented";
    OHAO_LOG_WARNING("HLSL compilation requested but not implemented: " + filename);
    return result;
}

VkShaderModule ShaderCompiler::createShaderModule(const std::vector<uint32_t>& spirvCode,
                                                  OhaoVkDevice* device) const {
    if (!device || spirvCode.empty()) {
        return VK_NULL_HANDLE;
    }
    
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirvCode.size() * sizeof(uint32_t);
    createInfo.pCode = spirvCode.data();
    
    VkShaderModule shaderModule;
    VkResult result = vkCreateShaderModule(device->getDevice(), &createInfo, nullptr, &shaderModule);
    
    if (result != VK_SUCCESS) {
        OHAO_LOG_ERROR("Failed to create Vulkan shader module");
        return VK_NULL_HANDLE;
    }
    
    return shaderModule;
}

bool ShaderCompiler::validateSPIRV(const std::vector<uint32_t>& spirvCode) const {
    if (spirvCode.empty()) {
        return false;
    }
    
    // Basic validation - check SPIR-V magic number
    if (spirvCode.size() < 5 || spirvCode[0] != 0x07230203) {
        return false;
    }
    
    // TODO: Add more thorough SPIR-V validation using spirv-val
    return true;
}

std::string ShaderCompiler::disassembleSPIRV(const std::vector<uint32_t>& spirvCode) const {
    // TODO: Implement SPIR-V disassembly using spirv-dis
    return "SPIR-V disassembly not implemented";
}

ShaderCompiler::ShaderReflection ShaderCompiler::reflectShader(const std::vector<uint32_t>& spirvCode) const {
    ShaderReflection reflection;
    
#ifdef OHAO_USE_SPIRV_CROSS
    try {
        spirv_cross::Compiler compiler(spirvCode);
        spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        
        // Reflect stage inputs
        for (const auto& input : resources.stage_inputs) {
            ShaderReflection::Variable var;
            var.name = input.name;
            var.location = compiler.get_decoration(input.id, spv::DecorationLocation);
            var.type = compiler.get_type(input.type_id).basetype == spirv_cross::SPIRType::Float ? "float" : "other";
            reflection.inputs.push_back(var);
        }
        
        // Reflect stage outputs
        for (const auto& output : resources.stage_outputs) {
            ShaderReflection::Variable var;
            var.name = output.name;
            var.location = compiler.get_decoration(output.id, spv::DecorationLocation);
            var.type = compiler.get_type(output.type_id).basetype == spirv_cross::SPIRType::Float ? "float" : "other";
            reflection.outputs.push_back(var);
        }
        
        // Reflect uniform buffers
        for (const auto& uniform : resources.uniform_buffers) {
            ShaderReflection::Variable var;
            var.name = uniform.name;
            var.binding = compiler.get_decoration(uniform.id, spv::DecorationBinding);
            var.set = compiler.get_decoration(uniform.id, spv::DecorationDescriptorSet);
            var.type = "uniform_buffer";
            reflection.uniforms.push_back(var);
        }
        
        // Reflect storage buffers
        for (const auto& storage : resources.storage_buffers) {
            ShaderReflection::Variable var;
            var.name = storage.name;
            var.binding = compiler.get_decoration(storage.id, spv::DecorationBinding);
            var.set = compiler.get_decoration(storage.id, spv::DecorationDescriptorSet);
            var.type = "storage_buffer";
            reflection.storageBuffers.push_back(var);
        }
        
        // Reflect images
        for (const auto& image : resources.storage_images) {
            ShaderReflection::Variable var;
            var.name = image.name;
            var.binding = compiler.get_decoration(image.id, spv::DecorationBinding);
            var.set = compiler.get_decoration(image.id, spv::DecorationDescriptorSet);
            var.type = "storage_image";
            reflection.images.push_back(var);
        }
        
        // Reflect samplers
        for (const auto& sampler : resources.sampled_images) {
            ShaderReflection::Variable var;
            var.name = sampler.name;
            var.binding = compiler.get_decoration(sampler.id, spv::DecorationBinding);
            var.set = compiler.get_decoration(sampler.id, spv::DecorationDescriptorSet);
            var.type = "sampler";
            reflection.samplers.push_back(var);
        }
        
        // For compute shaders, get work group size
        if (compiler.get_execution_model() == spv::ExecutionModelGLCompute) {
            auto& type = compiler.get_type(compiler.get_entry_points_and_stages()[0].second);
            reflection.computeWorkGroupSize.x = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
            reflection.computeWorkGroupSize.y = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1);
            reflection.computeWorkGroupSize.z = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2);
        }
        
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Shader reflection failed: " + std::string(e.what()));
    }
#else
    OHAO_LOG_WARNING("Shader reflection not available (SPIRV-Cross not enabled)");
#endif
    
    return reflection;
}

std::string ShaderCompiler::shaderStageToString(ShaderStage stage) const {
    switch (stage) {
        case ShaderStage::VERTEX: return "vertex";
        case ShaderStage::FRAGMENT: return "fragment";
        case ShaderStage::GEOMETRY: return "geometry";
        case ShaderStage::TESSELLATION_CONTROL: return "tess_control";
        case ShaderStage::TESSELLATION_EVALUATION: return "tess_eval";
        case ShaderStage::COMPUTE: return "compute";
        default: return "unknown";
    }
}

void ShaderCompiler::addBuiltinDefines(std::vector<std::string>& defines, ShaderStage stage) const {
    // Add stage-specific defines
    defines.push_back("OHAO_" + shaderStageToString(stage));
    
    // Add Vulkan-specific defines
    defines.push_back("VULKAN 1");
    defines.push_back("OHAO_ENGINE 1");
    
    // Add optimization level define
    defines.push_back("OHAO_OPTIMIZATION_LEVEL " + std::to_string(optimizationLevel));
}

std::string ShaderCompiler::preprocessSource(const std::string& source,
                                            const std::vector<std::string>& defines,
                                            const std::vector<std::string>& includePaths) const {
    std::ostringstream result;
    
    // Add #version directive if not present
    if (source.find("#version") == std::string::npos) {
        result << "#version 450 core\n";
    }
    
    // Add defines
    for (const auto& define : defines) {
        if (define.find(' ') != std::string::npos) {
            // Define with value
            size_t spacePos = define.find(' ');
            std::string name = define.substr(0, spacePos);
            std::string value = define.substr(spacePos + 1);
            result << "#define " << name << " " << value << "\n";
        } else {
            // Define without value
            result << "#define " << define << "\n";
        }
    }
    
    // Add a separator comment
    result << "// --- Original source code ---\n";
    result << source;
    
    return result.str();
}

} // namespace ohao