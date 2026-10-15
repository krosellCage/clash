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

    Out.Position = float4(In.Position, 1.0f);
    Out.Color = In.Color;

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    return In.Color;
}
