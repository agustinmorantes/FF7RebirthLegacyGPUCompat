// Mesh shader in the shape the game uses (128 threads, one meshlet per group, thread t writes
// vertex t and primitive t), run through ms2cs.py. Group g covers the g-th half of the target.
// b0 is visible to the mesh stage and to the pixel stage with different contents, so the
// proxy's compute root signature must move the pixel one out of the way.
cbuffer MsCB : register(b0) { float4 msData; }  // (x offset, id base, shade, bottom y)
struct V {
    float4 pos : SV_Position;
    nointerpolation uint id : ID;
    float shade : SHADE;
    float2 extra[2] : TEXCOORD0;
    nointerpolation int signedValue : SIGNEDVAL;
    float3 normal : NORMAL;
};

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSMain(uint gid : SV_GroupID, uint tid : SV_GroupThreadID, out vertices V verts[128],
            out indices uint3 tris[64]) {
    SetMeshOutputCounts(4, 2);
    if (tid < 4) {
        V v;
        v.pos = float4(msData.x + gid + (tid >> 1) - 1.0f, (tid & 1) ? 1.0f : msData.w, 0.5f, 1.0f);
        v.id = gid + (uint)msData.y;
        v.shade = msData.z;
        v.extra[0] = float2(0.25f, 0.125f);
        v.extra[1] = float2(0.5f, 0.0625f);
        v.signedValue = -7;
        v.normal = float3(0.125f, 0.25f, 0.5f);
        verts[tid] = v;
    }
    if (tid < 2) tris[tid] = tid ? uint3(2, 1, 3) : uint3(0, 1, 2);
}

cbuffer PsCB : register(b0) { float4 psData; }
float4 PSMain(V v) : SV_Target {
    float extra = v.extra[0].x + v.extra[0].y + v.extra[1].x + v.extra[1].y - 0.9375f;
    return float4(v.id / 255.0f + v.normal.x + v.normal.y + v.normal.z - 0.875f,
                  v.shade + extra, psData.x + v.signedValue + 7, 1.0f);
}

// Writes a root constant through a root UAV: checks that the proxy restores the compute state.
cbuffer CsCB : register(b0) { uint csValue; }
RWByteAddressBuffer CsOut : register(u0);
[numthreads(1, 1, 1)] void CSMain() { CsOut.Store(0, csValue); }
