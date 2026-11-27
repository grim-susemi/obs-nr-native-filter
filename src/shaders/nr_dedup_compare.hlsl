// SPDX-License-Identifier: GPL-3.0-only
Texture2D<float4> current : register(t0);
Texture2D<float4> reference : register(t1);
RWByteAddressBuffer overBand : register(u0);
groupshared uint groupCount;

[RootSignature("DescriptorTable(SRV(t0,numDescriptors=2)), UAV(u0)")]
[numthreads(16,16,1)]
void main(uint3 pixel : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    if (lane == 0) groupCount = 0;
    GroupMemoryBarrierWithGroupSync();
    uint width, height;
    current.GetDimensions(width, height);
    if (pixel.x < width && pixel.y < height) {
        int3 a = int3(round(current.Load(int3(pixel.xy,0)).rgb * 255.0));
        int3 b = int3(round(reference.Load(int3(pixel.xy,0)).rgb * 255.0));
        uint3 outside = uint3(abs(a-b) > 3);
        uint count = outside.x + outside.y + outside.z;
        if (count) InterlockedAdd(groupCount, count);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && groupCount) overBand.InterlockedAdd(0, groupCount);
}
