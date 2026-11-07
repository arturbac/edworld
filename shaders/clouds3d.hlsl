// edworld (PoC, volumetric clouds): the clouds as a volume between two spheres over the planet, marched along the ray
// of each pixel. Drawn after the game's shell of the atmosphere, with what that draw has bound (see clouds.hlsl). The
// sphere drawn only gives every pixel of the planet's neighbourhood one ray: it stands above the layer and only its far
// half is kept, which a ray meets once whether the camera is outside it or inside. t0 is a copy of the scene's depth,
// reversed as the game keeps it, so a ray ends at the ground or at the ship.
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
  float cloud_bottom;  // over the surface, as a part of the planet's radius
  float cloud_top;
  float cloud_cover;   // 0 clear to 1 overcast
  float cloud_light;   // how much of the sun they give back
  float cloud_steps;
  float cloud_density;
  float cloud_opacity; // the most they cover what is behind them
  float cloud_top_gain; // how much lighter they are seen from above, against a light ground
  float cloud_min_light; // the star's light they get at least, in the game's units; its hue stays
  float cloud_under_gain; // how much lighter they are seen from under the layer, against a day's sky
  float2 cloud_pad;
  };

Texture2D<float> scene_depth : register(t0);

struct vs_in
  {
  float3 pos : POSITION;
  };

struct ps_in
  {
  float4 pos : SV_POSITION;
  float3 view : TEXCOORD0;  // from the camera to this point of the sphere
  };

ps_in vs_main(vs_in input)
  {
  ps_in output;
  float scale = frame_rows[126].x * (1.f + cloud_top) * 1.01f / length(object_rows[9].xyz);
  float4 p = float4(input.pos * scale, 1.f);
  precise float x = dot(object_rows[9], p);
  precise float y = dot(object_rows[10], p);
  precise float z = dot(object_rows[11], p);
  output.pos = frame_rows[270] * x + frame_rows[271] * y + frame_rows[272] * z + frame_rows[273];
  output.view = float3(x, y, z);
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

float fbm(float3 p, int octaves)
  {
  float sum = 0.f;
  float amplitude = 0.5f;
  [loop]
  for(int i = 0; i != octaves; ++i)
    {
    sum += amplitude * noise(p);
    p = p * 2.03f + 7.1f;
    amplitude *= 0.5f;
    }
  return sum;
  }

float4 ps_main(ps_in input) : SV_TARGET
  {
  float3 ray = normalize(input.view);
  float3 centre = float3(object_rows[9].w, object_rows[10].w, object_rows[11].w);
  if(dot(input.view - centre, ray) <= 0.f)
    discard;

  float planet = frame_rows[126].x;
  float bottom = planet * (1.f + cloud_bottom);
  float top = planet * (1.f + cloud_top);

  float along = dot(ray, centre);
  float3 aside = centre - ray * along;
  float miss = dot(aside, aside);
  if(miss >= top * top)
    discard;
  float half_top = sqrt(top * top - miss);
  float enter = max(along - half_top, 0.f);
  float leave = along + half_top;
  if(leave <= 0.f)
    discard;
  if(miss < bottom * bottom)
    {
    float half_bottom = sqrt(bottom * bottom - miss);
    if(along - half_bottom > 0.f)
      leave = min(leave, along - half_bottom);  // from above: down to the layer's floor
    else
      enter = max(enter, along + half_bottom);  // from under it: up from the floor
    }
  if(leave <= enter)
    discard;

  float scene = scene_depth.Load(int3(input.pos.xy, 0));
  float3 seed = frac(planet / float3(977.f, 1543.f, 2111.f)) * 50.f;
  float inverse_square = 1.f / dot(object_rows[9].xyz, object_rows[9].xyz);
  float3 sun = -frame_rows[291].xyz;
  float edge = 0.62f - 0.3f * cloud_cover;

  // the swirls of the weather change slowly: worked out once for the ray
  float3 middle = ray * (enter + leave) * 0.5f - centre;
  float3 middle_dir = normalize(object_rows[9].xyz * middle.x + object_rows[10].xyz * middle.y + object_rows[11].xyz * middle.z);
  float3 warp = float3(fbm(middle_dir * 3.f + seed, 4), fbm(middle_dir * 3.f + seed + 11.f, 4), fbm(middle_dir * 3.f + seed + 23.f, 4));

  int steps = (int)cloud_steps;
  float step_length = (leave - enter) / cloud_steps;
  float through = step_length / (top - bottom);
  // a different start at each pixel turns the bands of the steps into a fine grain
  float t = enter + step_length * hash(float3(input.pos.xy, 1.f));
  float3 light = 0.f;
  float clear = 1.f;

  [loop]
  for(int i = 0; i != steps; ++i, t += step_length)
    {
    float3 p = ray * t;
    float4 clip = frame_rows[270] * p.x + frame_rows[271] * p.y + frame_rows[272] * p.z + frame_rows[273];
    if(clip.z / clip.w < scene)  // the depth is reversed: the scene stands nearer than this point
      break;
    float3 from_centre = p - centre;
    float distance_to_centre = length(from_centre);
    float height = saturate((distance_to_centre - bottom) / (top - bottom));
    float3 turned = (object_rows[9].xyz * from_centre.x + object_rows[10].xyz * from_centre.y + object_rows[11].xyz * from_centre.z)
                    * inverse_square;
    float3 dir = normalize(turned);
    float3 place = dir * 7.f + seed + warp * 3.5f + float3(0.f, height * 0.4f, 0.f);
    float shape = fbm(place, 5) - edge;
    float profile = saturate(height * 5.f) * saturate((1.f - height) * 2.5f);
    float density = saturate(shape * 2.5f);
    density *= density * profile;
    if(density > 0.001f)
      {
      float optical = density * cloud_density * through;
      float taken = 1.f - exp(-optical);
      float3 normal = from_centre / distance_to_centre;
      float day = saturate(dot(normal, sun) * 0.85f + 0.15f);
      // the tops catch the sun, the undersides are in the shade of what is above them
      float shade = lerp(0.35f, 1.f, height);
      light += clear * taken * day * shade;
      clear *= 1.f - taken;
      if(clear < 0.01f)
        break;
      }
    }

  // each gain comes in over a layer's height: from the floor up to the top none, above and under it the two
  float height_of_eye = (length(centre) - bottom) / (top - bottom);
  float gain = lerp(lerp(cloud_under_gain, 1.f, saturate(height_of_eye)), cloud_top_gain, saturate(height_of_eye - 1.f));
  float3 star = frame_rows[293].xyz;
  float strongest = max(max(star.r, star.g), max(star.b, 0.001f));
  star *= max(1.f, cloud_min_light / strongest);
  float3 colour = light * star * cloud_light * frame_rows[90].y * gain;
  float fade = frame_rows[126].z;
  float weight = fade * cloud_opacity;
  return float4(colour * weight, (1.f - clear) * weight);
  }
