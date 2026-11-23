#include "McTerrainVertexFactory.h"

#include "MeshBatch.h"
#include "MeshMaterialShader.h"
#include "MeshDrawShaderBindings.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FMcTerrainFaceParameters, "McTerrainFaces");

void FMcTerrainVertexFactory::BindFaceData(FRHIShaderResourceView* Records, uint32 Mode)
{
    FMcTerrainFaceParameters Parameters;
    Parameters.Records = Records;
    Parameters.Mode = Mode;
    FaceUniform = TUniformBufferRef<FMcTerrainFaceParameters>::CreateUniformBufferImmediate(Parameters, UniformBuffer_MultiFrame);
}

bool FMcTerrainVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
    return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
        && FLocalVertexFactory::ShouldCompilePermutation(Parameters);
}

class FMcTerrainVertexFactoryParameters final : public FLocalVertexFactoryShaderParametersBase
{
    DECLARE_TYPE_LAYOUT(FMcTerrainVertexFactoryParameters, NonVirtual);

public:
    void Bind(const FShaderParameterMap& ParameterMap)
    {
        FLocalVertexFactoryShaderParametersBase::Bind(ParameterMap);
        FaceUniform.Bind(ParameterMap, TEXT("McTerrainFaces"));
    }

    void GetElementShaderBindings(const FSceneInterface* Scene, const FSceneView* View, const FMeshMaterialShader* Shader,
        EVertexInputStreamType InputStreamType, ERHIFeatureLevel::Type FeatureLevel,
        const FVertexFactory* VertexFactory, const FMeshBatchElement& BatchElement,
        FMeshDrawSingleShaderBindings& ShaderBindings, FVertexInputStreamArray& VertexStreams) const
    {
        const auto* Factory = static_cast<const FMcTerrainVertexFactory*>(VertexFactory);
        GetElementShaderBindingsBase(Scene, View, Shader, InputStreamType, FeatureLevel,
            Factory, BatchElement, Factory->GetUniformBuffer(), ShaderBindings, VertexStreams);
        ShaderBindings.Add(FaceUniform, Factory->FaceUniform.GetReference());
    }

private:
    LAYOUT_FIELD(FShaderUniformBufferParameter, FaceUniform);
};

IMPLEMENT_TYPE_LAYOUT(FMcTerrainVertexFactoryParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FMcTerrainVertexFactory, SF_Vertex, FMcTerrainVertexFactoryParameters);
#if RHI_RAYTRACING
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FMcTerrainVertexFactory, SF_RayHitGroup, FMcTerrainVertexFactoryParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FMcTerrainVertexFactory, SF_Compute, FMcTerrainVertexFactoryParameters);
#endif
IMPLEMENT_VERTEX_FACTORY_TYPE(FMcTerrainVertexFactory, "/MinecraftUEViewer/Private/McTerrainVertexFactory.ush",
    EVertexFactoryFlags::UsedWithMaterials | EVertexFactoryFlags::SupportsStaticLighting |
    EVertexFactoryFlags::SupportsDynamicLighting | EVertexFactoryFlags::SupportsPrecisePrevWorldPos |
    EVertexFactoryFlags::SupportsPositionOnly | EVertexFactoryFlags::SupportsPrimitiveIdStream |
    EVertexFactoryFlags::SupportsRayTracing | EVertexFactoryFlags::SupportsManualVertexFetch |
    EVertexFactoryFlags::SupportsLumenMeshCards | EVertexFactoryFlags::SupportsTriangleSorting);

