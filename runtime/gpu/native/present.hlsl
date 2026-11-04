Texture2D<float4> source : register(t0);
RWTexture2D<float4> destination : register(u0);
cbuffer Extent : register(b0) { uint2 sourceSize; uint2 destinationSize; }
[numthreads(8,8,1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if(any(thread.xy>=destinationSize))return;
    uint2 pixel=min(uint2((thread.xy+0.5)*sourceSize/destinationSize),sourceSize-1);
    destination[thread.xy]=float4(source.Load(int3(pixel,0)).rgb,1);
}
