// A compute shader for the catalog's shape test: cb1 of three rows, a structured input at t2, a UAV at u0, 8x4x1.
cbuffer params : register(b1)
  {
  float4 rows[3];
  };
StructuredBuffer<float4> points : register(t2);
RWStructuredBuffer<float4> heights : register(u0);
[numthreads(8, 4, 1)]
void main(uint3 id : SV_DispatchThreadID)
  {
  heights[id.x + id.y * 8] = points[id.x] * rows[id.y % 3];
  }
