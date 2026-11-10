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
  float cloud_game_fade;    // 1: they fade with the game's own atmosphere's shell, 0: never
  float cloud_mode;         // 0 near or far by the eye's height, 1 always near, 2 always far
  float cloud_far_steps;    // the steps from afar: the same shape, fewer of them
  float cloud_near_from;    // all near under this height of the eye over the surface, as a part of the planet's radius
  float cloud_near_to;      // all far over this one, a blend between
  float cloud_debug;        // 1: each pixel worked out painted magenta, 2: the game's fade, green at 1, red at 0
  float cloud_view_width;   // the picture's width in pixels
  float cloud_hide_px;      // no clouds while the planet is narrower on the screen than this, all of them from the next
  float cloud_full_px;
  float cloud_max_steps;    // the most steps a ray takes, where its way through the layer is long
  float cloud_edge_hide_px; // none out of the planet's edge while the layer is thinner on the screen than this, all from the next
  float cloud_edge_full_px;
  float cloud_edge_band_px; // on the disc the same within this many pixels of the edge, all further in
  float cloud_haze;         // from under the layer and in it, a cloud this far from the eye covers e times less, as a part
                            // of the planet's radius; 0 = never less
  // the atmosphere's kind (edworld_eht): the planet whose kind is known, its clouds, how far they have come in
  float cloud_kind_radius;  // that planet's radius in metres; 0 = none, every planet the clouds above
  float cloud_kind_weight;  // 0 the clouds above .. 1 the kind's
  float cloud_kind_bottom;
  float cloud_kind_top;
  float cloud_kind_cover;
  float cloud_kind_opacity;
  float cloud_kind_detail;
  float cloud_tint_r;       // the kind's tint, times the star's light
  float cloud_tint_g;
  float cloud_tint_b;
  float cloud_pad;
  // the storms (edworld_eht) of that planet: each its centre on the unit sphere in the planet's frame and its size (an
  // angle), then the twists at their middles (radians), four to a row, then how many
  float4 cyclone_at[8];
  float4 cyclone_twist[2];
  float cyclone_count;
  float3 cyclone_pad;
  };

Texture2D<float> scene_depth : register(t0);
Texture2D<float> blue_noise : register(t2);  // 64x64, repeated: where each pixel's steps start

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
  float scale = frame_rows[126].x * (1.f + max(cloud_top, cloud_kind_top)) * 1.01f / length(object_rows[9].xyz);
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

// the storms (v1.2b, 2026-11-10): d turned about each storm's centre as a Rankine vortex - the core out to 0.35 of the
// storm's size turns as one, so the clouds are not drawn out there, and the turn falls as 1/x^2 past it, where the
// clouds come curving in; the sample turned back the way the storm turns them. eye: how much of the clouds is left at
// d (a small clear eye, its edge uneven), wall: what the dense disc of clouds around the eye adds to the shape, inner:
// how far into a storm d is, for its finer texture. weight 0 = no storms (another planet)
float3 storms(float3 d, float weight, inout float eye, inout float wall, inout float inner)
  {
  if(weight <= 0.f)
    return d;
  [loop]
  for(int i = 0; i < (int)cyclone_count; ++i)
    {
    float4 c = cyclone_at[i];
    float apart = length(d - c.xyz) / c.w;  // the chord over the size: about the angle's share for a small storm
    if(apart > 2.5f)
      continue;
    float profile = (apart < 0.35f ? 1.f : (0.35f * 0.35f) / (apart * apart)) * saturate((2.5f - apart) * 2.f);
    float twist = -cyclone_twist[i >> 2][i & 3] * weight * profile;
    float s = sin(twist);
    float co = cos(twist);
    d = d * co + cross(c.xyz, d) * s + c.xyz * dot(c.xyz, d) * (1.f - co);
    if(apart < 0.25f)
      {
      float seen = apart * (1.f + 0.4f * (fbm(d * 40.f + 5.3f, 2) - 0.375f));
      eye = min(eye, lerp(1.f, smoothstep(0.10f, 0.16f, seen), weight));
      }
    float rise = smoothstep(0.12f, 0.2f, apart);
    float fall = smoothstep(0.45f, 0.7f, apart);
    wall = max(wall, 0.45f * weight * rise * (1.f - fall));
    inner = max(inner, weight * exp(-apart * apart));
    }
  return d;
  }

float4 ps_main(ps_in input) : SV_TARGET
  {
  float3 ray = normalize(input.view);
  float3 centre = float3(object_rows[9].w, object_rows[10].w, object_rows[11].w);
  if(dot(input.view - centre, ray) <= 0.f)
    discard;

  float planet = frame_rows[126].x;
  // this planet's kind known: its clouds, coming in over cloud_kind_weight; any other planet the clouds above
  float kind = abs(planet - cloud_kind_radius) < 4.f ? cloud_kind_weight : 0.f;
  float layer_cover = lerp(cloud_cover, cloud_kind_cover, kind);
  float layer_opacity = lerp(cloud_opacity, cloud_kind_opacity, kind);
  float layer_detail = lerp(cloud_detail, cloud_kind_detail, kind);
  float3 tint = lerp(float3(1.f, 1.f, 1.f), float3(cloud_tint_r, cloud_tint_g, cloud_tint_b), kind);
  float bottom = planet * (1.f + lerp(cloud_bottom, cloud_kind_bottom, kind));
  float top = planet * (1.f + lerp(cloud_top, cloud_kind_top, kind));

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

  // a planet of a few dozen pixels across shows no clouds, only lumps standing out of its edge: none until it is larger
  // rows 270..272 take a place in the world's axes: the screen's x of one metre is the length of their x, whichever way
  // the ship looks. Their x alone also held the turn of the view and the size followed the ship's heading (2026-11-08)
  float x_scale = length(float3(frame_rows[270].x, frame_rows[271].x, frame_rows[272].x));
  float across = 2.f * planet / max(length(centre), planet) * x_scale * cloud_view_width * 0.5f;
  float shown = smoothstep(cloud_hide_px, cloud_full_px, across);
  // out of the edge the layer stands as thin as its height on the screen, a pixel or two of the half size picture whose
  // lumps flickered as the ship moved, while the disc's middle stayed still (2026-11-08): none there while it is that thin,
  // and on the disc less near the edge, where the rays go through the layer edge on
  float pixels_a_metre = across / (2.f * planet);
  float edge_kept = smoothstep(cloud_edge_hide_px, cloud_edge_full_px, (top - bottom) * pixels_a_metre);
  float from_edge = (bottom - sqrt(miss)) * pixels_a_metre;
  shown *= lerp(edge_kept, 1.f, saturate(from_edge / cloud_edge_band_px));
  if(shown <= 0.f)
    discard;
  // the game's own fade of its atmosphere's shell: on the way in the clouds faded out for a moment with it, where the
  // game changes the planet's detail (2026-11-08, about 8.3 Mm out)
  float game_fade = cloud_game_fade >= 0.5f ? frame_rows[126].z : 1.f;
  if(cloud_debug >= 1.5f)
    return float4(0.8f * (1.f - saturate(frame_rows[126].z)), 0.8f * saturate(frame_rows[126].z), 0.f, 0.8f);
  if(cloud_debug >= 0.5f)
    return float4(0.6f, 0.f, 0.6f, 0.6f);

  // near and far are the same shape: from afar only fewer steps and no fine shape, which is out of its reach there
  // anyway, so nothing jumps where they meet. Whole steps: a blend of their count changes the grain, not the clouds
  float eye_height = length(centre) / planet - 1.f;
  float near_weight = cloud_mode >= 1.5f ? 0.f : cloud_mode >= 0.5f ? 1.f : 1.f - smoothstep(cloud_near_from, cloud_near_to, eye_height);
  float scene = scene_depth.Load(int3(input.pos.xy * cloud_pixel, 0));
  float3 seed = frac(planet / float3(977.f, 1543.f, 2111.f)) * 50.f;
  float inverse_square = 1.f / dot(object_rows[9].xyz, object_rows[9].xyz);
  float3 sun = -frame_rows[291].xyz;
  float edge = 0.62f - 0.3f * layer_cover;

  // the swirls of the weather change slowly: worked out once for the ray. Not from the middle of its way through the
  // layer, which jumps at the limb and as the eye enters the layer, and the whole shape with it
  float3 middle = ray * max(anchor, 0.f) - centre;
  float3 middle_dir = normalize(object_rows[9].xyz * middle.x + object_rows[10].xyz * middle.y + object_rows[11].xyz * middle.z);
  // the wind: the large shape by the height, whose layers part slowly; the fine lumps all at the mean speed, or a
  // difference of 150 km/h would draw a lump of 3 km out into a streak in a minute. Each planet its own way
  float wind_angle = (seed.x > 25.f ? 1.f : -1.f) * cloud_wind_seconds / planet;
  float mean_angle = wind_angle * (cloud_wind_low + cloud_wind_high) * 0.5f;
  middle_dir = blown(middle_dir, mean_angle);
  float middle_eye = 1.f;
  float middle_wall = 0.f;
  float middle_inner = 0.f;
  middle_dir = storms(middle_dir, kind, middle_eye, middle_wall, middle_inner);
  float3 warp = float3(fbm(middle_dir * 3.f + seed, 4), fbm(middle_dir * 3.f + seed + 11.f, 4), fbm(middle_dir * 3.f + seed + 23.f, 4));

  // the steps follow the way's length through the layer: a step no longer than half the smallest lumps (the shape's
  // last octave, about a 118th of the radius), at the edge seen edge on many, across the layer few - at least the count
  // set for near or far, at most cloud_max_steps
  float least_steps = lerp(cloud_far_steps, cloud_steps, near_weight);
  float wanted_steps = (leave - enter) / (planet / 236.f);
  int steps = max(1, (int)ceil(clamp(wanted_steps, least_steps, max(cloud_max_steps, least_steps))));
  float step_length = (leave - enter) / steps;
  float through = step_length / (top - bottom);
  // a different start at each pixel turns the bands of the steps into a fine grain. Blue noise: a hash of the pixel's
  // place gave clumps of specks on a wide target, interleaved gradient noise fine slanting stripes where the steps are
  // long (2026-11-07, at the horizon), moved each frame they stayed without a temporal anti-aliasing, as in the game's own
  float start = blue_noise.Load(int3(uint2(input.pos.xy) & 63u, 0));
  // along the ray the place on the screen and the place in the planet's frame change in proportion to t: worked out
  // once for the ray, each step only adds
  float4 clip_ray = frame_rows[270] * ray.x + frame_rows[271] * ray.y + frame_rows[272] * ray.z;
  float3 turned_ray = (object_rows[9].xyz * ray.x + object_rows[10].xyz * ray.y + object_rows[11].xyz * ray.z) * inverse_square;
  float3 turned_centre = (object_rows[9].xyz * centre.x + object_rows[10].xyz * centre.y + object_rows[11].xyz * centre.z) * inverse_square;
  // the most the fine shape near the eye can add to a point's shape
  float detail_most = max(0.f, (0.875f - 0.44f) * layer_detail - cloud_detail_gap);
  // the air between: from under the layer a ray low over the horizon went through clouds 100 km and more away as dense
  // as those overhead, and ended at the outline of the game's hazy far ground in a sharp line (2026-11-09). Far ones
  // cover less, as in the game's own haze; from a layer's height over it up the same as before, and from orbit
  float height_of_eye = (length(centre) - bottom) / (top - bottom);
  float hazed = cloud_haze > 0.f ? 1.f - saturate(height_of_eye - 1.f) : 0.f;
  float haze_length = max(cloud_haze, 1e-6f) * planet;
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
    float eye = 1.f;
    float wall = 0.f;
    float inner = 0.f;
    dir = storms(dir, kind, eye, wall, inner);
    float3 place = blown(dir, wind_angle * lerp(cloud_wind_low, cloud_wind_high, height)) * 7.f + seed + warp * 3.5f + float3(0.f, height * 0.4f, 0.f);
    // the shape above has lumps of tens of km: fine from orbit, one smooth fog from under them. Near the eye lumps of a
    // few km cut it, where the steps are short enough to show them
    float near = saturate(1.f - t / cloud_detail_range) * saturate(cloud_detail_size * 2.f / step_length) * near_weight;
    float head = fbm_head(place);
    float most = head + 0.21875f - edge + wall;
    if(most + (layer_detail > 0.f && most > -0.2f ? detail_most * near : 0.f) <= 0.f)
      continue;
    float shape = fbm_tail(place, head) - edge + wall;
    if(inner > 0.01f)  // a storm's finer texture, wound with it
      shape += (fbm(dir * 20.f + seed, 3) - 0.44f) * 0.6f * inner;
    float sharpness = 2.5f;
    if(layer_detail > 0.f && near > 0.f && shape > -0.2f)
      {
      float3 fine = blown(dir, mean_angle) * (distance_to_centre / cloud_detail_size) + seed;
      shape += ((fbm(fine, 3) - 0.44f) * layer_detail - cloud_detail_gap) * near;
      sharpness = lerp(2.5f, cloud_detail_edge, near);
      }
    float profile = saturate(height * 5.f) * saturate((1.f - height) * 2.5f);
    float density = saturate(shape * sharpness);
    density *= density * profile * eye;
    if(density > 0.001f)
      {
      float optical = density * cloud_density * through;
      float taken = (1.f - exp(-optical)) * lerp(1.f, exp(-t / haze_length), hazed);
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
  float gain = lerp(lerp(cloud_under_gain, 1.f, saturate(height_of_eye)), cloud_top_gain, saturate(height_of_eye - 1.f));
  float3 star = frame_rows[293].xyz;
  float strongest = max(max(star.r, star.g), max(star.b, 0.001f));
  star *= max(1.f, cloud_min_light / strongest);
  float3 colour = light * star * tint * cloud_light * frame_rows[90].y * gain;
  float weight = game_fade * layer_opacity * shown;
  return float4(colour * weight, (1.f - clear) * weight);
  }
