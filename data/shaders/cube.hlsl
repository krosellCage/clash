// Milestone 5: the spinning cube (Luna 6.4 - 6.6, his Box demo).
//
// Two constant buffers, split by how often they change (Luna 6.6.3): the
// object's World matrix, and the camera's ViewProj for the whole frame.
// register(b0) / register(b1) are the slots the root signature fills.
//
// Plain float4x4, read column by column - which is why RendererRender
// transposes each matrix on its way in, as Luna does.
cbuffer ObjectConstants : register(b0)
{
    float4x4 World;
};

cbuffer PassConstants : register(b1)
{
    float4x4 ViewProj;
};

struct vertex_in
{
    float3 Position : POSITION;
    float4 Color : COLOR;
};

struct vertex_out
{
    float4 Position : SV_POSITION;
    float4 Color : COLOR;
};

vertex_out VS(vertex_in In)
{
    vertex_out Out;

    // Row vectors, as in DirectXMath: the vector on the LEFT.  Local space
    // to world space (where the cube is), then to clip space (where the
    // camera sees it).  The GPU does the divide by w afterwards.
    float4 WorldPosition = mul(float4(In.Position, 1.0f), World);
    Out.Position = mul(WorldPosition, ViewProj);
    Out.Color = In.Color;

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    return In.Color;
}
