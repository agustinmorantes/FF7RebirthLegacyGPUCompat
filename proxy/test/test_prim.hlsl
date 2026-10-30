// Mesh shader with per-primitive outputs in the shape the game's environment shaders use (128 threads,
// 128 vertices, 64 primitives, SV_CullPrimitive and a PRIMITIVEID attribute), run through ms2cs.py.
// Group g draws a band of 20 quads (40 triangles over 42 shared vertices) in rows 8g..8g+8 of an 80x16
// target. Triangle t is culled when t % 3 == 1 or t == 33 (second cull-mask word); group 2 culls all.
struct V {
    float4 pos : SV_Position;
    nointerpolation uint tc : TEXCOORD0;
};
struct P {
    nointerpolation uint pid : PRIMITIVEID;
    bool cull : SV_CullPrimitive;
};

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSMain(uint gid : SV_GroupID, uint tid : SV_GroupThreadID, out vertices V verts[128],
            out indices uint3 tris[64], out primitives P prims[64]) {
    SetMeshOutputCounts(42, 40);
    if (tid < 42) {
        float x = (tid >> 1) / 10.0f - 1.0f;
        float y = 1.0f - gid - ((tid & 1) ? 1.0f : 0.0f);
        verts[tid].pos = float4(x, y, 0.5f, 1.0f);
        verts[tid].tc = gid + 5;
    }
    if (tid < 40) {
        uint q = tid >> 1;
        tris[tid] = (tid & 1) ? uint3(2 * q + 1, 2 * q + 3, 2 * q + 2) : uint3(2 * q, 2 * q + 1, 2 * q + 2);
        prims[tid].pid = gid * 1000 + tid * 7;
        prims[tid].cull = gid == 2 || tid % 3 == 1 || tid == 33;
    }
}

struct VIn {
    float4 pos : SV_Position;
    nointerpolation uint tc : TEXCOORD0;
    nointerpolation uint pid : PRIMITIVEID;
};
uint2 PSMain(VIn v) : SV_Target { return uint2(v.pid, v.tc); }
