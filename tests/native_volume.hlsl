cbuffer SharedConstants : register(b2, space4) { uint4 sharedData[128]; }
Texture3D<float4> volumes[] : register(t0, space1);
float4 vs_main(float4 position : POSITION0) : SV_Position { return position; }
float4 ps_main(float4 position : SV_Position) : SV_Target0
{
    uint x=uint(position.x),y=uint(position.y);
    return volumes[sharedData[8].x].Load(int4(x%4,y,x/4,0));
}
