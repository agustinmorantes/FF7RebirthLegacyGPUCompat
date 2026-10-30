// Reads an R16_FLOAT typed buffer as Buffer<half> (needs native 16-bit ops until the proxy
// swaps in the fix16.py conversion) and writes the values doubled.
Buffer<half> In : register(t0);
RWBuffer<float> Out : register(u0);

[numthreads(4, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    Out[i] = (float)In[i] * 2.0f;
}
