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
  float cloud_detail;       // how much the fine shape near the eye cuts the clouds, 0 = none
  float cloud_detail_size;  // its largest lumps, in metres
  float cloud_detail_range; // how far from the eye it reaches, in metres
  float cloud_detail_edge;  // how sharp a cloud's edge is near the eye
  float cloud_detail_gap;   // how much clear sky it opens near the eye
  float cloud_wind_seconds; // since the first cloud of this game
  float cloud_wind_low;     // m/s at the layer's floor
  float cloud_wind_high;    // m/s at its top
  float cloud_pixel;        // pixels of the picture to a pixel drawn: 2 when the clouds are drawn at half size
  float cloud_pad;
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

// around the planet's Y axis, the way the clouds drift
float3 blown(float3 d, float angle)
  {
  float c = cos(angle);
  float s = sin(angle);
  return float3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
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

// fbm(p, 5) in two parts: the first two octaves, then the last three. The last three add less than 0.21875 (noise is
// under 1), so a point whose first two leave it that far under a cloud's edge is clear and needs no more
float fbm_head(inout float3 p)
  {
  float sum = 0.5f * noise(p);
  p = p * 2.03f + 7.1f;
  sum += 0.25f * noise(p);
  p = p * 2.03f + 7.1f;
  return sum;
  }

float fbm_tail(float3 p, float sum)
  {
  sum += 0.125f * noise(p);
  p = p * 2.03f + 7.1f;
  sum += 0.0625f * noise(p);
  p = p * 2.03f + 7.1f;
  sum += 0.03125f * noise(p);
  return sum;
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
  // where the ray meets the floor, or comes nearest to it past the limb: moves smoothly with the ray and the eye
  float anchor = along;
  if(miss < bottom * bottom)
    {
    float half_bottom = sqrt(bottom * bottom - miss);
    if(along - half_bottom > 0.f)
      {
      leave = min(leave, along - half_bottom);  // from above: down to the layer's floor
      anchor = along - half_bottom;
      }
    else
      {
      enter = max(enter, along + half_bottom);  // from under it: up from the floor
      anchor = along + half_bottom;
      }
    }
  if(leave <= enter)
    discard;

  float scene = scene_depth.Load(int3(input.pos.xy * cloud_pixel, 0));
  float3 seed = frac(planet / float3(977.f, 1543.f, 2111.f)) * 50.f;
  float inverse_square = 1.f / dot(object_rows[9].xyz, object_rows[9].xyz);
  float3 sun = -frame_rows[291].xyz;
  float edge = 0.62f - 0.3f * cloud_cover;

  // the swirls of the weather change slowly: worked out once for the ray. Not from the middle of its way through the
  // layer, which jumps at the limb and as the eye enters the layer, and the whole shape with it
  float3 middle = ray * max(anchor, 0.f) - centre;
  float3 middle_dir = normalize(object_rows[9].xyz * middle.x + object_rows[10].xyz * middle.y + object_rows[11].xyz * middle.z);
  // the wind: the large shape by the height, whose layers part slowly; the fine lumps all at the mean speed, or a
  // difference of 150 km/h would draw a lump of 3 km out into a streak in a minute. Each planet its own way
  float wind_angle = (seed.x > 25.f ? 1.f : -1.f) * cloud_wind_seconds / planet;
  float mean_angle = wind_angle * (cloud_wind_low + cloud_wind_high) * 0.5f;
  middle_dir = blown(middle_dir, mean_angle);
  float3 warp = float3(fbm(middle_dir * 3.f + seed, 4), fbm(middle_dir * 3.f + seed + 11.f, 4), fbm(middle_dir * 3.f + seed + 23.f, 4));

  int steps = (int)cloud_steps;
  float step_length = (leave - enter) / cloud_steps;
  float through = step_length / (top - bottom);
  // a different start at each pixel turns the bands of the steps into a fine grain; interleaved gradient noise, as a hash
  // of the pixel's place gave clumps of specks on a wide target
  float start = frac(52.9829189f * frac(dot(input.pos.xy, float2(0.06711056f, 0.00583715f))));
  // along the ray the place on the screen and the place in the planet's frame change in proportion to t: worked out
  // once for the ray, each step only adds
  float4 clip_ray = frame_rows[270] * ray.x + frame_rows[271] * ray.y + frame_rows[272] * ray.z;
  float3 turned_ray = (object_rows[9].xyz * ray.x + object_rows[10].xyz * ray.y + object_rows[11].xyz * ray.z) * inverse_square;
  float3 turned_centre = (object_rows[9].xyz * centre.x + object_rows[10].xyz * centre.y + object_rows[11].xyz * centre.z) * inverse_square;
  // the most the fine shape near the eye can add to a point's shape
  float detail_most = max(0.f, (0.875f - 0.44f) * cloud_detail - cloud_detail_gap);
  float t = enter + step_length * start;
  float3 light = 0.f;
  float clear = 1.f;

  [loop]
  for(int i = 0; i != steps; ++i, t += step_length)
    {
    float4 clip = clip_ray * t + frame_rows[273];
    if(clip.z < scene * clip.w)  // the depth is reversed: the scene stands nearer than this point
      break;
    float3 from_centre = ray * t - centre;
    float distance_to_centre = length(from_centre);
    float height = saturate((distance_to_centre - bottom) / (top - bottom));
    float3 dir = normalize(turned_ray * t - turned_centre);
    float3 place = blown(dir, wind_angle * lerp(cloud_wind_low, cloud_wind_high, height)) * 7.f + seed + warp * 3.5f + float3(0.f, height * 0.4f, 0.f);
    // the shape above has lumps of tens of km: fine from orbit, one smooth fog from under them. Near the eye lumps of a
    // few km cut it, where the steps are short enough to show them
    float near = saturate(1.f - t / cloud_detail_range) * saturate(cloud_detail_size * 2.f / step_length);
    float head = fbm_head(place);
    float most = head + 0.21875f - edge;
    if(most + (cloud_detail > 0.f && most > -0.2f ? detail_most * near : 0.f) <= 0.f)
      continue;
    float shape = fbm_tail(place, head) - edge;
    float sharpness = 2.5f;
    if(cloud_detail > 0.f && near > 0.f && shape > -0.2f)
      {
      float3 fine = blown(dir, mean_angle) * (distance_to_centre / cloud_detail_size) + seed;
      shape += ((fbm(fine, 3) - 0.44f) * cloud_detail - cloud_detail_gap) * near;
      sharpness = lerp(2.5f, cloud_detail_edge, near);
      }
    float profile = saturate(height * 5.f) * saturate((1.f - height) * 2.5f);
    float density = saturate(shape * sharpness);
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
