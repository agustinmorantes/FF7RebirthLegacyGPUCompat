struct V { float4 pos : SV_Position; nointerpolation uint id : ID; float2 value : COLOR; };
float2 Values(uint id) {
    return float2(f16tof32((id & 1) ? 0x8001u : 0x3555u),
                  f16tof32((id & 2) ? 0x7bffu : 0xbc00u));
}
[outputtopology("triangle")]
[numthreads(4, 2, 1)]
void MSMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex,
            out vertices V verts[4], out indices uint3 tris[2]) {
    SetMeshOutputCounts(4, 2);
    uint tile = gid.x + 2 * gid.y + 4 * gid.z;
    if (tid < 4) {
        V v;
        v.pos = float4(-1.0f + (tile + (tid >> 1)) / 6.0f, (tid & 1) ? 1 : -1, 0.5f, 1);
        v.id = tile + 1;
        v.value = Values(v.id);
        verts[tid] = v;
    }
    if (tid < 2) tris[tid] = tid ? uint3(2, 1, 3) : uint3(0, 1, 2);
}
uint PSMain(V v) : SV_Target { return all(asuint(v.value) == asuint(Values(v.id))) ? v.id : 0xdeadbeefu; }

struct RawV { float4 pos : SV_Position; float4 bits : BITS; uint4 words : WORDS; };
[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void MSBits(uint tid : SV_GroupIndex, out vertices RawV verts[4], out indices uint3 tris[2]) {
    SetMeshOutputCounts(4, 2);
    RawV v;
    v.pos = float4(0, 0, 0.5f, 1);
    v.bits = asfloat(uint4(0x80000000u, 0x00000001u, 0x7fc12345u, 0xff800000u));
    v.words = uint4(0, 0xffffffffu, 0x80000000u, 0x12345678u);
    verts[tid] = v;
    if (tid < 2) tris[tid] = tid ? uint3(2, 1, 3) : uint3(0, 1, 2);
}
