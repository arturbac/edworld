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
  float cloud_size_var;     // the lumps' size noised by region: their scale times 1 +- this; 0 = the same everywhere
  // the storms (edworld_eht) of that planet: each its centre on the unit sphere in the planet's frame and its size (an
  // angle), then the twists at their middles (radians), four to a row, then how many
  float4 cyclone_at[8];
  float4 cyclone_twist[2];
  float cyclone_count;
  float cyclone_tower;      // how many times the layer's height the storms reach, the rest keeping its lower part
  // the lumps by the layer's thickness and the weather (edworld_eht, design v2) of that planet
  float cloud_kind_scale;   // the shape's scale on the unit sphere, 0 = 7 as before
  float cloud_kind_detail_size; // the fine shape's largest lumps in metres, 0 = cloud_detail_size
  float cloud_kind_lean;    // how high the lumps reach, 0.55 flat sheets .. 1 towers; 0 = all the layer as before
  float cloud_weather;      // how far the weather moves a cloud's edge at most, 0 = no weather
  float cloud_kind_rossby;  // the planet's R/L, which the weather follows
  float cloud_v2_pad;
  // the aurora (design v1, 2026-11-15) of that planet, over the clouds on its night side; strength 0 = none
  float aurora_strength;    // the kind's yield times aurora_gain and the substorm's brightness, changing over minutes
  float aurora_level;       // how stormy, 0..9 as the Kp index
  float aurora_dance;       // how much it moves in seconds, 0..2
  float aurora_shift;       // the oval's boundary moved by the substorm, degrees
  float3 aurora_low;        // the light low in a curtain
  float aurora_flicker;     // 1: small patches flicker a few times a second
  float3 aurora_high;       // and high in it
  float aurora_field;       // 1: the planet has a field of its own, 0: a faint glow over the night side only
  float3 aurora_axis;       // the dipole, in the planet's frame
  float aurora_floor;       // the shell's floor and ceiling over the surface, as parts of the radius
  float aurora_ceiling;
  float3 aurora_edge;       // nitrogen's pink lower edge, 0 = none
  // the snow (PoC, src/snow.h) near the ground on foot: crystals under a full cloud and in a clear sky (0 = none), how
  // fast they fall (m/s), then where the eye walked east and north since a reference fix (metres, wrapped)
  float snow_under;
  float snow_clear;
  float snow_fall;
  float snow_pad0;
  float snow_east;
  float snow_north;
  float snow_pad1;
  float snow_pad2;
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
  float shell = max(max(cloud_top, cloud_kind_top), aurora_strength > 0.f ? aurora_ceiling : 0.f);
  float scale = frame_rows[126].x * (1.f + shell) * 1.01f / length(object_rows[9].xyz);
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
// how far into a storm d is, for its finer texture. v1.4s/v1.5 (2026-11-11): the eye a funnel, half as wide at the
// layer's floor as at its middle and 1.4 times at its top (height 0..1), with a dense eyewall at its edge all the way
// down; wallness: how much d is in that wall, inward: the way to the eye's middle there (both for its light)
float3 storms(float3 d, float weight, float height, inout float eye, inout float wall, inout float inner,
              inout float wallness, inout float3 inward)
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
    if(apart < 0.4f)
      {
      float grow = 0.5f + 0.9f * height;
      float seen = apart * (1.f + 0.4f * (fbm(d * 40.f + 5.3f, 2) - 0.375f));
      eye = min(eye, lerp(1.f, smoothstep(0.06f * grow, 0.10f * grow, seen), weight));
      float ring = (seen - 0.115f * grow) / 0.07f;
      float in_wall = weight * exp(-ring * ring);
      wall = max(wall, 0.35f * in_wall);
      if(in_wall > wallness)
        {
        wallness = in_wall;
        inward = normalize(c.xyz - d * dot(d, c.xyz) + 1e-6f);
        }
      }
    float rise = smoothstep(0.12f, 0.2f, apart);
    float fall = smoothstep(0.45f, 0.7f, apart);
    wall = max(wall, 0.45f * weight * rise * (1.f - fall));
    inner = max(inner, weight * exp(-apart * apart));
    }
  return d;
  }

// a storm's cloud at p (from the planet's centre, the world's axes) by its large shape alone, for the shadow it casts:
// what the step's density is, in two octaves instead of five
float storm_density(float3 p, float bottom, float top, float kind, float tower, float3 seed, float3 warp, float edge,
                    float wind_angle, float scale, float weather_shift)
  {
  float distance_to_centre = length(p);
  if(distance_to_centre < bottom || distance_to_centre > top)
    return 0.f;
  float height = (distance_to_centre - bottom) / (top - bottom);
  float3 d = normalize(object_rows[9].xyz * p.x + object_rows[10].xyz * p.y + object_rows[11].xyz * p.z);
  float eye = 1.f, wall = 0.f, inner = 0.f, wallness = 0.f;
  float3 inward = 0.f;
  d = storms(d, kind, height, eye, wall, inner, wallness, inward);
  float3 place = blown(d, wind_angle * lerp(cloud_wind_low, cloud_wind_high, height)) * scale + seed + warp * 3.5f + float3(0.f, height * 0.4f, 0.f);
  float stormy = saturate(inner * 1.5f);
  float shape = fbm_head(place) + 0.11f - (edge + weather_shift * stormy) + wall;  // the last three octaves' mean
  float low = min(height * tower, 1.f);
  float h = lerp(low, height, stormy);
  if(height * tower >= 1.f && inner < 0.3f)
    return 0.f;
  float density = saturate(shape * 2.5f);
  return density * density * saturate(h * 5.f) * saturate((1.f - h) * 2.5f) * eye;
  }

// the clouds along the ray, premultiplied; none (0) where the ray misses them, so the aurora may still be drawn
// the weather (design v2) at a place of the planet: -1 clear .. 1 overcast. A slow turn for the planet's size (R/L
// under 0.3): clouds over the day side about the point under the star, held to the ground (ground_dir); a faster one:
// bands by latitude and patches drifting with the wind (middle_dir, the place blown)
float weather_at(float3 ground_dir, float3 middle_dir, float3 seed, float3 sun_model)
  {
  float rossby = cloud_kind_rossby;
  if(rossby < 0.3f)
    return 2.f * smoothstep(-0.2f, 0.8f, dot(ground_dir, sun_model)) - 1.f;
  float latitude = degrees(asin(clamp(middle_dir.y, -1.f, 1.f)));
  float period = rossby <= 4.f ? 50.f : 100.f / (rossby * 0.5f);
  float band = cos(latitude / period * 6.2831853f) * (1.f - 0.7f * smoothstep(60.f, 85.f, abs(latitude)));
  float patch = (fbm(middle_dir * max(rossby * 1.5f, 1.5f) + seed + 57.f, 2) - 0.375f) / 0.375f;
  return clamp(0.5f * band + 0.5f * patch, -1.f, 1.f);
  }

float4 clouds_along(ps_in input, float3 ray, float3 centre)
  {
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
    return 0.f;
  float half_top = sqrt(top * top - miss);
  float enter = max(along - half_top, 0.f);
  float leave = along + half_top;
  if(leave <= 0.f)
    return 0.f;
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
    return 0.f;

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
    return 0.f;
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
  float3 ground_dir = middle_dir;  // held to the planet, for the weather under the star
  middle_dir = blown(middle_dir, mean_angle);
  float middle_eye = 1.f;
  float middle_wall = 0.f;
  float middle_inner = 0.f;
  float middle_wallness = 0.f;
  float3 middle_inward = 0.f;
  middle_dir = storms(middle_dir, kind, 0.5f, middle_eye, middle_wall, middle_inner, middle_wallness, middle_inward);
  // the storms' tower and the sun in the planet's frame, for the eyewall's light
  float tower = cyclone_count > 0.5f && kind > 0.f ? max(cyclone_tower, 1.f) : 1.f;
  float3 sun_model = normalize(object_rows[9].xyz * sun.x + object_rows[10].xyz * sun.y + object_rows[11].xyz * sun.z);
  float3 warp = float3(fbm(middle_dir * 3.f + seed, 4), fbm(middle_dir * 3.f + seed + 11.f, 4), fbm(middle_dir * 3.f + seed + 23.f, 4));
  // the lumps' size noised by region (Artur, 2026-11-11): the shape's scale times 1 +- cloud_size_var from a slow noise
  // of the ray's middle (fbm of 2 octaves spans 0..0.75, mean 0.375), so one region has larger lumps and another
  // smaller instead of one size over the whole planet; once a ray, as the warp
  float grow = 1.f + cloud_size_var * (fbm(middle_dir * 1.5f + seed + 31.f, 2) - 0.375f) / 0.375f;
  // design v2 (2026-11-12): the lumps' scale by the layer's thickness of the kind instead of a 7th of the radius, so a
  // small planet's lumps are larger for its size and a large one's smaller; the fine shape near the eye by it as well,
  // noised by region as the coarse one. Coming in with the kind as its other clouds
  float shape_scale = lerp(7.f, cloud_kind_scale > 0.f ? cloud_kind_scale : 7.f, kind) * grow;
  float detail_size = lerp(cloud_detail_size, cloud_kind_detail_size > 0.f ? cloud_kind_detail_size * grow : cloud_detail_size, kind);
  float dome = cloud_kind_lean > 0.f ? kind : 0.f;  // how much the lumps' own heights have come in
  // the weather (design v2): overcast and clear regions, a cloud's edge moved by up to cloud_weather, once a ray. A
  // slow turn for the planet's size (R/L under 0.3): clouds over the day side about the point under the star, where it
  // heats most, held to the ground, not the wind; a faster one: bands by latitude, the equator's and the middle
  // latitudes' overcast, about 25 degrees clear (narrower bands past R/L 4), and patches the size of the Rossby radius
  // drifting with the wind
  float weather_shift = 0.f;  // how far the weather moved the edge, given back in a storm, which brings its own clouds
  if(cloud_weather > 0.f && kind > 0.f)
    {
    float weather = weather_at(ground_dir, middle_dir, seed, sun_model);
    weather_shift = cloud_weather * weather * kind;
    edge -= weather_shift;
    }

  // the steps follow the way's length through the layer: a step no longer than half the smallest lumps (the shape's
  // last octave, about a 118th of the radius), at the edge seen edge on many, across the layer few - at least the count
  // set for near or far, at most cloud_max_steps
  float least_steps = lerp(cloud_far_steps, cloud_steps, near_weight);
  float wanted_steps = (leave - enter) / (planet / 236.f) * (max(shape_scale, 7.f) / 7.f);  // smaller lumps, shorter steps
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
    float wallness = 0.f;
    float3 inward = 0.f;
    dir = storms(dir, kind, height, eye, wall, inner, wallness, inward);
    float3 place = blown(dir, wind_angle * lerp(cloud_wind_low, cloud_wind_high, height)) * shape_scale + seed + warp * 3.5f + float3(0.f, height * 0.4f, 0.f);
    // the shape above has lumps of tens of km: fine from orbit, one smooth fog from under them. Near the eye lumps of a
    // few km cut it, where the steps are short enough to show them
    float near = saturate(1.f - t / cloud_detail_range) * saturate(detail_size * 2.f / step_length) * near_weight;
    float head = fbm_head(place);
    // a storm's clouds as before the weather: a storm in a clear band keeps its mass, one in an overcast band no more
    float stormy = saturate(inner * 1.5f);
    float step_edge = edge + weather_shift * stormy;
    float most = head + 0.21875f - step_edge + wall;
    if(most + (layer_detail > 0.f && most > -0.2f ? detail_most * near : 0.f) <= 0.f)
      continue;
    float shape = fbm_tail(place, head) - step_edge + wall;
    if(inner > 0.01f)  // a storm's finer texture, wound with it
      shape += (fbm(dir * 20.f + seed, 3) - 0.44f) * 0.6f * inner;
    float sharpness = 2.5f;
    if(layer_detail > 0.f && near > 0.f && shape > -0.2f)
      {
      float3 fine = blown(dir, mean_angle) * (distance_to_centre / detail_size) + seed;
      shape += ((fbm(fine, 3) - 0.44f) * layer_detail - cloud_detail_gap) * near;
      sharpness = lerp(2.5f, cloud_detail_edge, near);
      }
    // a storm towers through the layer, the clouds out of storms keep its lower part as before (tower 1: all of it)
    float low = min(height * tower, 1.f);
    float profile_height = lerp(low, height, stormy);
    // design v2: a lump as high as it is strong, a weak one a low flat sheet, a strong core up to the top (lean: the
    // kind's leaning to towers); the same column in a lower lump, or the shallow ones all but vanish from orbit. A
    // storm's clouds keep all the layer
    float reach = lerp(1.f, (0.3f + 0.7f * saturate(shape * 4.f)) * cloud_kind_lean, dome * (1.f - stormy));
    profile_height /= max(reach, 1e-3f);
    float profile = saturate(profile_height * 5.f) * saturate((1.f - profile_height) * 2.5f) / max(reach, 0.3f);
    if(tower > 1.f && height * tower >= 1.f && inner < 0.3f)
      profile = 0.f;
    float density = saturate(shape * sharpness);
    density *= density * profile * eye;
    if(density > 0.001f)
      {
      float optical = density * cloud_density * through * lerp(tower, 1.f, stormy);
      float taken = (1.f - exp(-optical)) * lerp(1.f, exp(-t / haze_length), hazed);
      float3 normal = from_centre / distance_to_centre;
      float day = saturate(dot(normal, sun) * 0.85f + 0.15f);
      // the eyewall by the way it faces the sun: the far side of the eye bright, the near one shaded
      if(wallness > 0.001f)
        day *= 1.f - 0.5f * wallness + 0.8f * wallness * saturate(-dot(inward, sun_model) * 0.5f + 0.5f);
      // in a storm the clouds' own shadow: what lies toward the sun, a quarter and most of the layer's height along it
      float own = 1.f;
      if(inner > 0.05f)
        {
        float span = top - bottom;
        float towards = storm_density(from_centre + sun * span * 0.25f, bottom, top, kind, tower, seed, warp, edge, wind_angle, shape_scale, weather_shift) * 0.25f
                        + storm_density(from_centre + sun * span * 0.8f, bottom, top, kind, tower, seed, warp, edge, wind_angle, shape_scale, weather_shift) * 0.55f;
        own = lerp(1.f, exp(-towards * 2.4f), saturate(inner * 4.f));
        }
      // the tops catch the sun, the undersides are in the shade of what is above them
      float shade = lerp(0.35f, 1.f, height);
      light += clear * taken * day * shade * own;
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

// the star's wind at the planet (aurora_star_wind in aurora.h): its activity by its class, read off the red over green
// of its light (one value per class in the game), times the light's level within 0.5..2 of 468
float aurora_star_wind()
  {
  float3 star = frame_rows[293].xyz;
  float red_over_green = star.r / max(star.g, 1e-6f);
  float activity = red_over_green >= 1.5f ? 0.3f : red_over_green >= 1.038f ? 1.f : red_over_green >= 1.022f ? 0.6f
                   : red_over_green >= 1.0075f ? 0.5f : red_over_green >= 0.9925f ? 0.3f : 0.1f;
  return activity * clamp(sqrt(max(star.g, 0.f) / 468.f), 0.5f, 2.f);
  }

// one arc of curtains, across degrees from its middle (aurora_across), thick degrees thin (a few km: the sheet the
// particles come down in), its own brightness here 0..1: a bright part is wider and stands lower. Along it by u (the
// rays' scale, about 3 km a unit) dense rays, each arc its own (seed): one in each cell of two lattices, about 1 km
// and 2.5 km, somewhere in its cell and soft across. Each ray has its own strength, changing smoothly over 2..6 s and
// alike in its neighbours (a group about 10 km long changing in about 6 s); as strong as it is, a ray is brighter,
// reaches higher and comes down deeper, as harder particles do, so the curtain is uneven at its foot and at its top;
// above its foot it fades up and ends softly. Between them a faint sheet low down, as strong as the group
float aurora_arc(float across, float thick, float thick_least, float h, float bright, float u, float seed, float time)
  {
  // widened to the step across it (or a ray would miss it), it is as much dimmer: its light stays a thin curtain's
  // (before 2026-11-16 a far one gave tens of times too much, a bright smooth band seen from orbit)
  float sheet_thick = thick * (0.7f + 0.6f * bright);
  float wide = max(sheet_thick, thick_least);
  float conserve = sheet_thick / wide;
  float sheet_across = exp(-pow(across / wide, 2.f));
  if(sheet_across < 0.002f)
    return 0.f;
  float group = saturate((noise(float3(u * 0.35f + seed, time * 0.15f, 17.f)) - 0.3f) * 2.2f);
  float base = 0.06f + 0.08f * (1.f - bright);
  float length_scale = 0.6f * (0.7f + 0.6f * bright);
  float light = 0.f;
  [unroll]
  for(int lattice = 0; lattice != 2; ++lattice)
    {
    float cells = lattice == 0 ? 3.f : 1.2f;
    float weight = lattice == 0 ? 1.f : 0.6f;
    float own = seed + (lattice == 0 ? 0.f : 31.f);
    float x = u * cells;
    float first_cell = floor(x);
    [unroll]
    for(int d = -1; d != 2; ++d)
      {
      float cell = first_cell + d;
      // one hash for the cell, its other values drawn from it
      float drawn = hash(float3(cell, own, 1.f));
      float rate = 0.17f + 0.33f * frac(drawn * 7.13f);
      // the noise at a whole cell and seed: only along the time, between two of its values
      float when = time * rate;
      float at = floor(when);
      float f = when - at;
      float changing = lerp(hash(float3(cell, own + 7.f, at)), hash(float3(cell, own + 7.f, at + 1.f)), f * f * (3.f - 2.f * f));
      float strength = saturate(0.6f * saturate((changing - 0.3f) * 2.2f) + 0.65f * group - 0.1f);
      float middle = cell + 0.2f + 0.6f * drawn;
      float width = 0.4f + 0.3f * frac(drawn * 13.71f);
      float foot = base - 0.07f * strength + 0.04f * (frac(drawn * 29.37f) - 0.5f);
      float reach = length_scale * (0.15f + 1.1f * strength);
      float above = h - foot;
      float along = saturate(above / 0.02f) * exp(-max(above, 0.f) / (0.5f * reach)) * saturate((foot + reach - h) / (0.3f * reach));
      light += weight * exp(-pow((x - middle) / width, 2.f)) * along * (0.1f + strength);
      }
    }
  float sheet = saturate((h - base) / 0.03f) * exp(-max(h - base, 0.f) / 0.1f) * 0.12f * group;
  return conserve * sheet_across * (light * 0.45f + sheet);
  }

// how far across an arc whose middle lies at magnetic latitude centre the point at north is, degrees, with the arc's
// own local folds, each arc its own (seed): a fine wiggle across it, about 20 along the oval, changing in about two
// seconds and drifting along it, 1.5 times sway (degrees) at most; and curls, where the arc winds into an S about
// a vortex: one of 12 cells round the oval holds one at a time, about a third of them in a quiet sky to 70% in a
// substorm, each growing and fading over 16 seconds, at most 2.5 radians round within twice sway. dance
// 0 keeps the arc still
float aurora_across(float north, float centre, float angle, float seed, float sway, float time, float dance)
  {
  float n = north - centre;
  if(dance <= 0.f)
    return n;
  float amount = 0.5f + 0.5f * saturate(dance);
  n -= amount * 1.5f * sway * (noise(float3(angle * 3.2f - time * 0.25f, seed, time * 0.4f)) * 2.f - 1.f);
  float c = angle * 1.90986f + seed;
  float cell = floor(c);
  float when = time / 16.f + hash(float3(cell, seed, 3.f));
  float3 k = float3(cell, floor(when), seed);
  if(hash(k) > 0.25f + 0.45f * saturate(dance))
    return n;
  // the vortex's middle along the cell and across the arc; along in degrees on the sphere at the arc's colatitude
  float middle = (hash(k + 2.f) - 0.5f) * sway;
  float2 p = float2((c - cell - 0.3f - 0.4f * hash(k + 1.f)) / 1.90986f * sin(radians(90.f - centre)) * 57.29578f, n - middle);
  float r = 2.f * sway;
  float turn = (hash(k + 3.f) > 0.5f ? 1.f : -1.f) * (1.f + 1.5f * saturate(dance)) * amount * sin(3.14159f * frac(when))
               * exp(-dot(p, p) / (r * r));
  return p.x * sin(turn) + p.y * cos(turn) + middle;
  }

// where along the oval an arc lights at all, 0..1: parts of it go dark, more of them as gaps is higher (0.38 about a
// quarter, 0.52 about a half); each arc its own (seed), drifting over minutes, faster in a substorm
float aurora_lit(float2 circle, float seed, float gaps, float time, float dance)
  {
  return smoothstep(gaps, gaps + 0.12f, noise(float3(circle * 3.f + seed, time * (0.02f + 0.1f * dance))));
  }

// the aurora's light at dir (the planet's frame, unit) and h (0 the shell's floor .. 1 its ceiling): the oval about the
// dipole, lower on the night side and with the storm, one arc in a quiet sky, two from level 3 and a third toward the
// pole as a strong substorm expands, each broken where it does not light, its curtains folded, their rays, the waves
// running along them in a substorm, the diffuse glow beside them; per unit of the shell's height
float3 aurora_glow(float3 dir, float h, float level, float3 sun_model, float time, float thick_least, float planet)
  {
  float night = saturate((0.1f - dot(dir, sun_model)) / 0.25f);
  if(night <= 0.f)
    return 0.f;
  float vertical = saturate(h / 0.04f) * exp(-h / 0.28f);
  float up = saturate((h - 0.15f) / 0.45f);
  float3 colour = lerp(aurora_low, aurora_high, up);
  if(dot(aurora_edge, aurora_edge) > 0.f)
    colour = lerp(colour, aurora_edge, exp(-h / 0.03f));
  if(aurora_field < 0.5f)  // no field of its own: a faint patchy glow over all the night side
    return colour * (0.04f * (0.5f + 0.5f * noise(dir * 4.f + time * 0.02f)) * vertical * night);

  float north = abs(degrees(asin(clamp(dot(dir, aurora_axis), -1.f, 1.f))));
  float3 noon = normalize(sun_model - aurora_axis * dot(sun_model, aurora_axis) + float3(1e-6f, 0.f, 0.f));
  float3 dusk = cross(aurora_axis, noon);
  // about the dipole from midnight: the seam of what is noised along the angle falls at noon, where nothing shows
  float2 about = float2(-dot(dir, noon), dot(dir, dusk));
  float angle = atan2(about.y, about.x);
  float2 circle = about / max(length(about), 1e-4f);
  float to_noon = -circle.x;
  float boundary = 72.f - 1.5f * level + (5.f + 0.5f * level) * to_noon + aurora_shift;
  float width = 3.f + 0.6f * level;
  float dance = aurora_dance;
  // the folds: slow ones always, and in a substorm ones that move in seconds
  float fold = (1.f + 0.2f * level) * (noise(float3(circle * 3.f, time * 0.05f)) * 2.f - 1.f)
               + dance * (0.6f * (noise(float3(circle * 5.f + 13.f, time * 0.18f)) * 2.f - 1.f) + 0.8f * sin(angle * 5.f - time * 0.65f));
  float centre = boundary + width * 0.3f + fold;
  // the curtains 4 km thin (2.8..5.2 by their brightness, never thinner than half a step), sways and curls as wide
  // as the old, thicker arcs
  float km = degrees(1000.f / planet);
  float thick = 4.f * km;
  float sway = 0.35f + 0.15f * level / 9.f;
  // the second arc from level 3, the third as a strong substorm expands (it dances most only then, from level 6)
  float second = 0.6f * saturate(level - 2.f);
  float third = 0.5f * saturate((dance - 0.7f) / 0.25f) * saturate(level - 5.f);
  float diffuse = 0.02f * exp(-pow((north - (boundary + width * 0.5f)) / (width * 0.6f), 2.f));
  // far from every arc: no noise to work out
  float off = north - centre;
  float reach = 3.f * max(thick * 1.3f, thick_least) + abs(fold) + 4.f * sway;
  if((off < -reach || off > reach + width * (third > 0.f ? 0.85f : 0.45f)) && diffuse < 0.002f)
    return 0.f;
  // the waves of brightness running along the arc, a few times as bright here and less there; the aurora as a whole
  // changes only with aurora_strength, over minutes
  float waves = clamp(1.f + dance * (0.9f * sin(angle * 14.f - time * 2.1f) + 0.6f * sin(angle * 23.f + time * 3.4f)), 0.3f, 3.f);
  // the gaps: about half the arcs dark in a quiet sky, a quarter in a strong one, more again as a substorm recovers
  // (the oval still pushed poleward, the dance fading)
  float recovering = saturate(aurora_shift / 3.f) * saturate(1.f - dance);
  float gaps = 0.52f - 0.14f * saturate((level - 2.f) / 5.f) + 0.1f * recovering;
  // the rays' scale, about 3 km a unit: the angle's length along the arc at its colatitude
  float ray_scale = planet * sin(radians(90.f - centre)) / 3000.f;
  float u = angle * ray_scale;
  // each arc: first how far across it the point is, and only near it (2.5 times its widest, where its curtain is
  // 0.002 of its middle) whether it lights there and its rays
  float arc = 0.f;
  float across1 = aurora_across(north, centre, angle, 0.f, sway, time, dance);
  if(abs(across1) < 2.5f * max(thick * 1.3f, thick_least))
    {
    float lit1 = aurora_lit(circle, 0.f, gaps, time, dance);
    if(lit1 > 0.f)
      arc += lit1 * aurora_arc(across1, thick, thick_least, h, saturate(lit1 * waves / 2.f), u, 0.f, time);
    }
  if(second > 0.f)
    {
    float across2 = aurora_across(north, centre + width * 0.45f + 0.5f * fold, angle, 23.f, sway * 1.3f, time, dance);
    if(abs(across2) < 2.5f * max(thick * 1.69f, thick_least))
      {
      float lit2 = aurora_lit(circle, 23.f, gaps, time, dance);
      if(lit2 > 0.f)
        arc += second * lit2 * aurora_arc(across2, thick * 1.3f, thick_least, h, saturate(lit2 * waves / 2.f), u, 23.f, time);
      }
    }
  if(third > 0.f)
    {
    float across3 = aurora_across(north, centre + width * 0.85f + 0.7f * fold, angle, 47.f, sway * 1.1f, time, dance);
    if(abs(across3) < 2.5f * max(thick * 1.43f, thick_least))
      {
      float lit3 = aurora_lit(circle, 47.f, gaps, time, dance);
      if(lit3 > 0.f)
        arc += third * lit3 * aurora_arc(across3, thick * 1.1f, thick_least, h, saturate(lit3 * waves / 2.f), u, 47.f, time);
      }
    }
  if(arc < 0.002f && diffuse < 0.002f)
    return 0.f;
  float flicker = 1.f;
  if(aurora_flicker > 0.5f && dance > 0.f && noise(float3(angle * 30.f + time, 5.3f, 0.f)) > 0.6f)
    flicker += min(dance, 1.f) * 0.15f * sin(time * 50.265f + hash(float3(floor(angle * 120.f), 1.f, 2.f)) * 6.283f);
  return colour * ((arc * waves * flicker + diffuse * vertical) * night);
  }

// the aurora along the ray: its shell between the floor and the ceiling, the part before the floor and the part past
// it unless the planet stands between, marched in a few steps, ending at the scene
float3 aurora_along(ps_in input, float3 ray, float3 centre, out float eye_under)
  {
  eye_under = 0.f;
  if(aurora_strength <= 0.f)
    return 0.f;
  float planet = frame_rows[126].x;
  float kind = abs(planet - cloud_kind_radius) < 4.f ? cloud_kind_weight : 0.f;
  if(kind <= 0.f)
    return 0.f;
  float floor_r = planet * (1.f + aurora_floor);
  float ceiling_r = planet * (1.f + aurora_ceiling);
  float along = dot(ray, centre);
  float3 aside = centre - ray * along;
  float miss = dot(aside, aside);
  if(miss >= ceiling_r * ceiling_r)
    return 0.f;
  float x_scale = length(float3(frame_rows[270].x, frame_rows[271].x, frame_rows[272].x));
  float across = 2.f * planet / max(length(centre), planet) * x_scale * cloud_view_width * 0.5f;
  if(across < cloud_hide_px)
    return 0.f;
  float half_ceiling = sqrt(ceiling_r * ceiling_r - miss);
  float a0 = max(along - half_ceiling, 0.f);
  float a1 = along + half_ceiling;
  float b0 = a1, b1 = a1;
  if(miss < floor_r * floor_r)
    {
    float half_floor = sqrt(floor_r * floor_r - miss);
    b0 = max(a0, along + half_floor);
    a1 = min(a1, along - half_floor);
    // the far part hidden behind the planet when the ray meets the ground ahead
    if(miss < planet * planet && along - sqrt(planet * planet - miss) > 0.f)
      b1 = b0;
    }
  float length_a = max(a1 - a0, 0.f);
  float length_b = max(b1 - b0, 0.f);
  if(length_a + length_b <= 0.f)
    return 0.f;

  float eye_height = length(centre) / planet - 1.f;
  eye_under = eye_height < lerp(cloud_top, cloud_kind_top, kind) ? 1.f : 0.f;
  float3 sun = -frame_rows[291].xyz;
  float3 sun_model = normalize(object_rows[9].xyz * sun.x + object_rows[10].xyz * sun.y + object_rows[11].xyz * sun.z);
  float inverse_square = 1.f / dot(object_rows[9].xyz, object_rows[9].xyz);
  float3 turned_ray = (object_rows[9].xyz * ray.x + object_rows[10].xyz * ray.y + object_rows[11].xyz * ray.z) * inverse_square;
  float3 turned_centre = (object_rows[9].xyz * centre.x + object_rows[10].xyz * centre.y + object_rows[11].xyz * centre.z) * inverse_square;
  // from under the shell only a dark sky shows it: the eye's own night
  float eye_night = 1.f;
  if(eye_height < aurora_floor)
    eye_night = saturate((0.05f - dot(normalize(-turned_centre), sun_model)) / 0.2f);
  if(eye_night <= 0.f)
    return 0.f;
  // the storm a little stronger under a strong star's wind
  float wind = aurora_star_wind();
  float level = min(aurora_level + 3.f * saturate((wind - 0.6f) / 1.4f), 9.f);
  float time = cloud_wind_seconds;
  float4 clip_ray = frame_rows[270] * ray.x + frame_rows[271] * ray.y + frame_rows[272] * ray.z;
  float scene = scene_depth.Load(int3(input.pos.xy * cloud_pixel, 0));
  float start = blue_noise.Load(int3(uint2(input.pos.xy) & 63u, 0));
  // more from in or near the shell, where the thin curtains stand close
  int steps = eye_height < aurora_ceiling * 1.5f ? 64 : 16;
  int steps_a = length_b <= 0.f ? steps : length_a <= 0.f ? 0 : clamp((int)round(steps * length_a / (length_a + length_b)), 1, steps - 1);
  float height = ceiling_r - floor_r;
  float3 light = 0.f;
  [loop]
  for(int i = 0; i != steps; ++i)
    {
    bool first = i < steps_a;
    float from = first ? a0 : b0;
    float span = first ? length_a : length_b;
    int n = first ? steps_a : steps - steps_a;
    float step_length = span / n;
    float t = from + step_length * ((first ? i : i - steps_a) + start);
    float4 clip = clip_ray * t + frame_rows[273];
    if(clip.z < scene * clip.w)  // the depth is reversed: the scene stands nearer
      {
      if(first)
        {
        i = steps_a - 1;  // on to the far part, which lies further still: none of it shows either
        continue;
        }
      break;
      }
    float3 p = turned_ray * t - turned_centre;
    float r = length(ray * t - centre);
    float h = (r - floor_r) / height;
    if(h < 0.f || h > 1.f)
      continue;
    // a curtain no thinner than the step across it, or it would flicker as the ship moves
    light += aurora_glow(normalize(p), h, level, sun_model, time, degrees(step_length / planet) * 0.5f, planet) * (step_length / height);
    }
  // the light in the game's units: 120 was what aurora_gain 3 gave over 40 at an F star, faint, and clear in the game
  // (2026-11-15, Dunkan from orbit); tuned by aurora_gain
  float strength = aurora_strength * wind * (0.3f + 0.7f * level / 9.f) * kind * eye_night;
  return light * strength * 120.f * frame_rows[90].y;
  }

// the snow (PoC, 2026-11-17): near the eye on foot, crystals of the air's own gas falling under the clouds, and the
// haze their many make further off. The space near the ground cut into cells of snow_cell metres, in the frame of the
// ground under the eye (east, north, up): in each cell one crystal or none, its place, size and turn from the cell's
// hash; the cells fall with the snow and drift with a slow wind, and move against the eye's walk on the ground
// (Status.json), so the crystals stay in the air as the eye turns and walks. The ray looks at snow_samples places from
// snow_near to snow_reach; at each, the crystal of that place's cell. Ice: translucent, giving back snow_albedo of the
// star's light in the colour of the kind's clouds, and flashing for a moment as it turns and a face catches the star
// (each its own beat, 1..5 a second; brighter towards the star); a speck of its own size (1..3 mm) drawn no smaller
// than half a pixel, soft, a short streak of one frame's way; one near enough to show its shape a soft hexagonal
// plate. None at night, none behind the ground (the scene's depth). How many: snow_under under a full cloud (the
// weather over the eye, as the clouds above have it), snow_clear in a clear sky; the haze over the way to the ground
// as dense as they are many (5 km to see at the most), hiding at most half of what is behind, grey in their colour
static const float snow_cell = 0.8f;
static const float snow_near = 1.5f;
static const float snow_reach = 30.f;
static const int snow_samples = 12;
static const float snow_albedo = 0.15f;
static const float snow_see_m = 5000.f;  // how far one sees in the densest snow
static const float snow_sky_m = 1500.f;  // the way of a ray that meets no ground
static const float snow_haze_most = 0.5f; // the haze never hides more than this (a white wall at 6 c, 2026-11-17)

float4 snow_along(ps_in input, float3 ray, float3 centre)
  {
  if(snow_under <= 0.f && snow_clear <= 0.f)
    return 0.f;
  float planet = frame_rows[126].x;
  float kind = abs(planet - cloud_kind_radius) < 4.f ? cloud_kind_weight : 0.f;
  // on that planet's ground only (Status.json says on foot outdoors, but the planet is the one whose kind was read)
  if(kind <= 0.f || length(centre) - planet > 5000.f)
    return 0.f;
  float inverse_square = 1.f / dot(object_rows[9].xyz, object_rows[9].xyz);
  float3 eye_model = -(object_rows[9].xyz * centre.x + object_rows[10].xyz * centre.y + object_rows[11].xyz * centre.z) * inverse_square;
  float3 up = normalize(eye_model);
  float3 east = cross(float3(0.f, 1.f, 0.f), up);
  east = dot(east, east) > 1e-6f ? normalize(east) : float3(1.f, 0.f, 0.f);
  float3 north = cross(up, east);
  float3 sun = -frame_rows[291].xyz;
  float3 sun_model = normalize(object_rows[9].xyz * sun.x + object_rows[10].xyz * sun.y + object_rows[11].xyz * sun.z);

  // how much cloud over the eye: the weather there, as the clouds above have it
  float cover = lerp(cloud_cover, cloud_kind_cover, kind);
  if(cloud_weather > 0.f)
    {
    float3 seed = frac(planet / float3(977.f, 1543.f, 2111.f)) * 50.f;
    cover = saturate(cover + 0.5f * cloud_weather * weather_at(up, up, seed, sun_model) / 0.3f);
    }
  float many = snow_under * smoothstep(0.35f, 0.75f, cover) + snow_clear;
  if(many <= 0.f)
    return 0.f;
  // the star's light on them, none once it is under the horizon; in the colour of the kind's clouds
  float day = saturate(dot(up, sun_model) * 4.f + 0.3f);
  if(day <= 0.f)
    return 0.f;
  float3 tint = lerp(float3(1.f, 1.f, 1.f), float3(cloud_tint_r, cloud_tint_g, cloud_tint_b), kind);
  float3 light = frame_rows[293].xyz * tint * day * kind * frame_rows[90].y;

  float3 ray_model = normalize((object_rows[9].xyz * ray.x + object_rows[10].xyz * ray.y + object_rows[11].xyz * ray.z) * inverse_square);
  float toward = saturate(dot(ray_model, sun_model));
  float4 clip_ray = frame_rows[270] * ray.x + frame_rows[271] * ray.y + frame_rows[272] * ray.z;
  float scene = scene_depth.Load(int3(input.pos.xy * cloud_pixel, 0));

  // the haze: the way to the ground (the depth is reversed: the scene nearer where clip.z < scene clip.w), found by
  // halving; a ray meeting none goes snow_sky_m
  float way = snow_sky_m;
  float4 far_clip = clip_ray * snow_sky_m + frame_rows[273];
  if(far_clip.z < scene * far_clip.w)
    {
    float lo = 0.f, hi = snow_sky_m;
    [unroll]
    for(int k = 0; k != 6; ++k)
      {
      float mid = 0.5f * (lo + hi);
      float4 c = clip_ray * mid + frame_rows[273];
      if(c.z < scene * c.w)
        hi = mid;
      else
        lo = mid;
      }
    way = lo;
    }
  // snow_under at its most (src/snow.h snow_most) sees snow_see_m
  float thick = 3.f / snow_see_m * many / 0.15f;
  float haze = snow_haze_most * (1.f - exp(-thick * way));

  // the ray in the ground's frame; the cells fall and drift with the time, and move against the eye's walk
  float3 dir = float3(dot(ray_model, east), dot(ray_model, north), dot(ray_model, up));
  float time = cloud_wind_seconds;
  float wind_angle = 6.2831853f * noise(float3(time * 0.01f, 3.1f, 7.7f));
  float2 drift = float2(cos(wind_angle), sin(wind_angle)) * (0.3f + 1.2f * noise(float3(time * 0.03f, 5.3f, 1.9f)));
  float3 shift = float3(snow_east + drift.x * time, snow_north + drift.y * time, snow_fall * time);
  // how a crystal moves against the eye: down with the fall, along with the wind; a streak of one frame's way
  float3 moving = -float3(drift, snow_fall);
  float streak = length(moving) / 60.f;
  moving = normalize(moving + float3(0.f, 0.f, -1e-4f));
  // a plate's own axes across the ray, for its hexagon
  float3 side = normalize(cross(dir, abs(dir.z) < 0.99f ? float3(0.f, 0.f, 1.f) : float3(1.f, 0.f, 0.f)));
  float3 rise = cross(side, dir);
  // a pixel's angle: a crystal is never drawn smaller than half a pixel, so it does not flicker
  float x_scale = length(float3(frame_rows[270].x, frame_rows[271].x, frame_rows[272].x));
  float pixel_angle = 2.f * cloud_pixel / max(cloud_view_width * x_scale, 1.f);

  float glow = 0.f;
  float start = blue_noise.Load(int3(uint2(input.pos.xy) & 63u, 0));
  [loop]
  for(int i = 0; i != snow_samples; ++i)
    {
    float t = snow_near * pow(snow_reach / snow_near, (i + start) / snow_samples);
    if(t > way)
      break;
    float3 at = shift + dir * t;
    float3 cell = floor(at / snow_cell);
    if(hash(cell + 17.f) > many)
      continue;
    float3 crystal = (cell + 0.15f + 0.7f * float3(hash(cell), hash(cell + 3.f), hash(cell + 7.f))) * snow_cell;
    float along = dot(crystal - shift, dir);
    if(along < snow_near || along > way)
      continue;
    float own = 0.001f + 0.002f * hash(cell + 11.f);
    float pixel = pixel_angle * along;
    float size = max(own, pixel * 0.5f);
    float3 off = shift + dir * along - crystal;
    // a soft speck drawn out along its way, its ends fading
    float on_way = dot(off, moving);
    float3 across = off - moving * on_way;
    float speck = exp(-2.f * dot(across, across) / (size * size)) * saturate(1.f - abs(on_way) / (streak * 0.5f + size));
    // near enough to show its shape: a soft hexagonal plate at its own turn
    float shown = smoothstep(2.f, 4.f, own / pixel);
    if(shown > 0.f)
      {
      float turn = 6.2831853f * hash(cell + 19.f);
      float2 q = float2(dot(off, side), dot(off, rise));
      q = abs(float2(q.x * cos(turn) - q.y * sin(turn), q.x * sin(turn) + q.y * cos(turn))) / own;
      float hexagon = max(q.x * 0.866f + q.y * 0.5f, q.y);
      speck = lerp(speck, 1.f - smoothstep(0.8f, 1.f, hexagon), shown);
      }
    // as bright as it covers of the pixel; a face catching the star flashes, each crystal its own beat
    float covers = saturate((own / size) * (own / size));
    float beat = 1.f + 4.f * hash(cell + 23.f);
    float flash = pow(saturate(sin(6.2831853f * (time * beat + hash(cell + 29.f)))), 40.f) * (0.5f + 4.f * pow(toward, 4.f));
    glow += speck * covers * (0.3f + 0.7f * hash(cell + 13.f)) * (0.2f + 0.3f * toward + flash);
    }
  float3 colour = light * (snow_albedo * glow * 12.f + haze * (0.1f + 0.13f * toward));
  return float4(colour, haze);
  }

float4 ps_main(ps_in input) : SV_TARGET
  {
  float3 ray = normalize(input.view);
  float3 centre = float3(object_rows[9].w, object_rows[10].w, object_rows[11].w);
  if(dot(input.view - centre, ray) <= 0.f)
    discard;
  float4 clouds = clouds_along(input, ray, centre);
  float eye_under;
  float3 aurora = aurora_along(input, ray, centre, eye_under);
  // the aurora only gives light: from under the clouds it shows through their gaps, from over them it stands in front
  float4 result = clouds + float4(aurora * lerp(1.f, 1.f - clouds.a, eye_under), 0.f);
  // the snow and its haze stand before the clouds, near the eye
  float4 snow = snow_along(input, ray, centre);
  result = snow + (1.f - snow.a) * result;
  if(all(result == 0.f))
    discard;
  return result;
  }
