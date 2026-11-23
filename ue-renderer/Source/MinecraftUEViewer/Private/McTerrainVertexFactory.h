#pragma once

#include "LocalVertexFactory.h"
#include "ShaderParameterStruct.h"

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FMcTerrainFaceParameters, )
    SHADER_PARAMETER_SRV(Buffer<uint>, Records)
    SHADER_PARAMETER(uint32, Mode)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

class FMcTerrainVertexFactory final : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FMcTerrainVertexFactory);

public:
    explicit FMcTerrainVertexFactory(ERHIFeatureLevel::Type FeatureLevel)
        : FLocalVertexFactory(FeatureLevel, "MinecraftTerrain") {}

    static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);

    void BindFaceData(FRHIShaderResourceView* Records, uint32 Mode);

    TUniformBufferRef<FMcTerrainFaceParameters> FaceUniform;
};

