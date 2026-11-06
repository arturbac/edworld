// edworld (PoC, clouds): a layer of clouds over a planet the game gives none. Drawn right after the game's own shell
// of the atmosphere, with what that draw has bound: its unit sphere, its object constants at b0 (rows 9..11 turn and
// scale the sphere and place it, in metres from the camera) and its frame constants at b1 - rows 270..273 the
// projection, 291 the light's direction, 293 its colour, 90 and 126 the game's exposure and fade, as its own cloud
// layer reads them. The clouds themselves are worked out here, from noise on the sphere.
cbuffer object : register(b0)
  {
  float4 object_rows[13];
  };

cbuffer frame : register(b1)
  {
  float4 frame_rows[336];
  };

cbuffer clouds : register(b4)
  {
  float cloud_radius;  // of the shell's radius
  float cloud_alpha;
  float cloud_cover;   // 0 clear to 1 overcast
  float cloud_pad;
  };

struct vs_in
  {
  float3 pos : POSITION;
  };

struct ps_in
  {
  float4 pos : SV_POSITION;
  float3 dir : TEXCOORD0;     // on the sphere, turning with the planet
  float3 normal : TEXCOORD1;  // in the frame the light is given in
  };

ps_in vs_main(vs_in input)
  {
  ps_in output;
  float4 p = float4(input.pos * cloud_radius, 1.f);
  precise float x = dot(object_rows[9], p);
  precise float y = dot(object_rows[10], p);
  precise float z = dot(object_rows[11], p);
  output.pos = frame_rows[270] * x + frame_rows[271] * y + frame_rows[272] * z + frame_rows[273];
  output.dir = normalize(input.pos);
  output.normal = float3(
    dot(object_rows[9].xyz, output.dir), dot(object_rows[10].xyz, output.dir), dot(object_rows[11].xyz, output.dir)
  );
  return output;
  }

float hash(float3 p)
  {
  p = frac(p * 0.3183099f + 0.1f);
  p *= 17.f;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
  }

float noise(float3 p)
  {
  float3 i = floor(p);
  float3 f = frac(p);
  f = f * f * (3.f - 2.f * f);
  return lerp(
    lerp(
      lerp(hash(i), hash(i + float3(1, 0, 0)), f.x),
      lerp(hash(i + float3(0, 1, 0)), hash(i + float3(1, 1, 0)), f.x),
      f.y
    ),
    lerp(
      lerp(hash(i + float3(0, 0, 1)), hash(i + float3(1, 0, 1)), f.x),
      lerp(hash(i + float3(0, 1, 1)), hash(i + float3(1, 1, 1)), f.x),
      f.y
    ),
    f.z
  );
  }

float fbm(float3 p)
  {
  float sum = 0.f;
  float amplitude = 0.5f;
  [unroll]
  for(int i = 0; i != 5; ++i)
    {
    sum += amplitude * noise(p);
    p = p * 2.03f + 7.1f;
    amplitude *= 0.5f;
    }
  return sum;
  }

float4 ps_main(ps_in input) : SV_TARGET
  {
  float3 dir = normalize(input.dir);
  // the warp drags the noise into streaks and swirls, as weather does
  float3 warp = float3(fbm(dir * 3.f), fbm(dir * 3.f + 11.f), fbm(dir * 3.f + 23.f));
  float density = fbm(dir * 7.f + warp * 3.5f);
  float edge = 0.62f - 0.3f * cloud_cover;
  density = smoothstep(edge, edge + 0.16f, density);

  // nearer than the layer the sphere is seen from inside, and a thin shell shows nothing true - it fades on the way down
  float3 centre = float3(object_rows[9].w, object_rows[10].w, object_rows[11].w);
  float radius = length(object_rows[9].xyz) * cloud_radius;
  float above = length(centre) / radius;
  float fade = smoothstep(1.005f, 1.05f, above);

  float3 normal = normalize(input.normal);
  // the wrap of the light past the terminator the game's own layer reads at row 128, which the game leaves empty
  // over a planet with no layer - its values at one that has
  float light = saturate(dot(normal, -frame_rows[291].xyz) * 0.85f + 0.15f);
  float3 colour = frame_rows[293].xyz * light * 0.9f * frame_rows[90].y;
  return float4(colour, density * cloud_alpha * fade * frame_rows[126].z);
  }
