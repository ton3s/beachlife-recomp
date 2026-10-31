#include "platform/path.h"
#include "platform/video.h"

#include "imgui.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace beachlife::platform
{
namespace
{
// Sharper upscaling of the game's 800x600 picture, compiled from source by Metal. A
// Catmull-Rom filter keeps edges that bilinear blurs, then contrast-adaptive sharpening
// restores fine detail. Both are limited by the nearby pixels, so edges do not get halos.
// Other graphics APIs, or BEACHLIFE_SCALER=linear, use the plain bilinear blit.
const char scaler_source[] = R"(#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; float2 uv; };
vertex Out main0(uint id [[vertex_id]]) {
    float2 p = float2((id << 1) & 2, id & 2);
    Out out;
    out.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    out.uv = p;
    return out;
}
)";
const char scaler_fragment_source[] = R"(#include <metal_stdlib>
using namespace metal;
struct In { float4 position [[position]]; float2 uv; };
struct Settings { float2 size; float sharpness; float active; float2 origin; float scale; float time; float4 areas[8]; int count; int edges; float night; float fall; };
static float3 at(texture2d<float> t, int2 p) {
    int2 m = int2(t.get_width(), t.get_height()) - 1;
    return t.read(uint2(clamp(p, int2(0), m))).rgb;
}
static float3 catmull(texture2d<float> t, float2 uv, float2 size, thread float3& low, thread float3& high) {
    float2 q = uv * size - 0.5;
    int2 b = int2(floor(q));
    float2 f = q - float2(b);
    float2 f2 = f * f, f3 = f2 * f;
    float2 w[4] = { -0.5*f3 + f2 - 0.5*f, 1.5*f3 - 2.5*f2 + 1.0, -1.5*f3 + 2.0*f2 + 0.5*f, 0.5*f3 - 0.5*f2 };
    float3 sum = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            sum += at(t, b + int2(x - 1, y - 1)) * w[x].x * w[y].y;
    low = float3(1); high = float3(0);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            float3 c = at(t, b + int2(x, y));
            low = min(low, c); high = max(high, c);
        }
    return sum;
}
// Teal sea has about as much green as blue; the blue of the interface and of pools does not.
static bool is_water(float3 c) { return c.b > c.r * 1.2 && c.g > c.r * 1.1 && c.b > 0.15 && c.b < c.g * 1.2; }
static bool is_sand(float3 c) { return c.r > c.g && c.g > c.b && c.r > 0.45 && c.r - c.b > 0.08 && c.r - c.g < 0.12; }
static float hash(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
static float noise(float2 p) {
    float2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + float2(1, 0)), f.x), mix(hash(i + float2(0, 1)), hash(i + float2(1, 1)), f.x), f.y);
}
// How much of this spot is water or sand, blended between texels so the edges stay smooth.
static float2 ground(texture2d<float> t, float2 game) {
    float2 q = game - 0.5;
    int2 b = int2(floor(q));
    float2 f = q - float2(b);
    float2 r = 0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            float3 c = at(t, b + int2(x, y));
            float w = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
            if (is_water(c)) r.x += w; else if (is_sand(c)) r.y += w;
        }
    return r;
}
// Rounds off stair-stepped diagonals, for the small sprites that have no HD art.
static bool clean_edge(texture2d<float> t, float2 game, thread float3& out) {
    int2 p = int2(floor(game));
    float2 f = fract(game) - 0.5;
    int2 d = int2(f.x < 0 ? -1 : 1, f.y < 0 ? -1 : 1);
    float3 e = at(t, p), a = at(t, p + int2(d.x, 0)), b = at(t, p + int2(0, d.y)), corner = at(t, p + d);
    if (distance(a, b) > 0.14 || distance(e, a) < 0.22 || distance(e, corner) < 0.1) return false;
    float reach = abs(f.x) + abs(f.y);
    out = mix(e, (a + b) * 0.5, smoothstep(0.42, 0.58, reach));
    return true;
}
// The waterfall painted into the first island's terrain. Only its bright water moves, so the
// rock stays put: streaks run down the falls, foam churns in the basin and a little mist rises.
static float3 waterfall(texture2d<float> t, sampler linear, float3 c, float2 game, float2 island, float time, float night, float2 size, float scale) {
    float edge = smoothstep(1625.0, 1665.0, island.x) * (1.0 - smoothstep(1905.0, 1945.0, island.x))
        * smoothstep(2395.0, 2425.0, island.y) * (1.0 - smoothstep(2750.0, 2795.0, island.y));
    if (edge <= 0.0) return c;
    // Water runs in vertical strands: reading along the strand ignores the rock's bright specks.
    float2 d = float2(0.0, 5.0 * scale) / size;
    float2 uv = game / size;
    float3 strand = (t.sample(linear, uv).rgb + t.sample(linear, uv + d).rgb + t.sample(linear, uv - d).rgb
        + t.sample(linear, uv + d * 2.0).rgb + t.sample(linear, uv - d * 2.0).rgb) / 5.0;
    float bright = dot(strand, float3(0.3, 0.55, 0.15));
    float tint = max(strand.r, max(strand.g, strand.b)) - min(strand.r, min(strand.g, strand.b));
    float dusk = 1.0 - night * 0.55;
    float water = smoothstep(0.42 * dusk, 0.66 * dusk, bright) * (1.0 - smoothstep(0.16, 0.34, tint));
    float day = 1.0 - night * 0.75;
    float basin = smoothstep(2640.0, 2700.0, island.y);
    // The falling water: two layers of long strands, moving at different speeds.
    float fast = noise(float2(island.x * 0.42, island.y * 0.035 - time * 2.6));
    float slow = noise(float2(island.x * 0.2 + 7.0, island.y * 0.02 - time * 1.5));
    float streak = smoothstep(0.42, 0.78, fast * 0.65 + slow * 0.35);
    float2 shift = float2(0.0, (fast - 0.5) * 3.0 * scale) / size;
    float3 moved = t.sample(linear, uv + shift).rgb * (0.82 + 0.3 * fast);
    moved = mix(moved, float3(0.96, 0.98, 1.0) * day, streak * 0.42);
    // The basin: foam boiling outward from under the falls.
    float2 from = island - float2(1765.0, 2675.0);
    float spread = length(from * float2(1.0, 1.8));
    float churn = noise(float2(from.x * 0.055, spread * 0.09 - time * 1.3))
        * 0.6 + noise(from * float2(0.13, 0.22) + float2(time * 0.5, -time * 0.8)) * 0.4;
    float foam = smoothstep(0.46, 0.76, churn);
    float3 boiling = mix(c * (0.9 + 0.2 * churn), float3(0.96, 0.98, 1.0) * day, foam * 0.58);
    float3 result = mix(moved, boiling, basin);
    float mask = mix(water, max(water, smoothstep(0.26 * dusk, 0.48 * dusk, bright) * 0.75), basin);
    // Mist drifts up from where the water lands, over rock as well.
    float away = length(from * float2(1.0 / 165.0, 1.0 / 62.0));
    float mist = (1.0 - smoothstep(0.35, 1.0, away)) * smoothstep(0.42, 0.8, noise(island * 0.03 + float2(time * 0.12, time * 0.42))) * 0.2 * day;
    return mix(mix(c, result, mask * edge), float3(0.95, 0.97, 1.0), mist * edge);
}
static float3 ocean(texture2d<float> t, sampler linear, float2 game, float2 island, float time, float night, float2 size, float scale) {
    float2 flow = float2(time * 0.9, time * 0.5);
    float2 wobble = float2(noise(island * 0.06 + flow * 0.3), noise(island * 0.06 - flow * 0.2 + 9.0)) - 0.5;
    float2 uv = (game + wobble * 1.6 * scale) / size;
    float2 d = 1.4 / size;
    // A soft read hides the dithered blocks of the original water.
    float3 base = (t.sample(linear, uv).rgb * 2.0 + t.sample(linear, uv + d).rgb + t.sample(linear, uv - d).rgb
        + t.sample(linear, uv + float2(d.x, -d.y)).rgb + t.sample(linear, uv + float2(-d.x, d.y)).rgb) / 6.0;
    float swell = noise(island * 0.035 + flow * 0.5) * 0.65 + noise(island * 0.09 - flow * 0.7) * 0.35;
    // Few, soft and drifting: tight bright points read as static on a large display.
    float glitter = smoothstep(0.62, 0.86, noise(island * 0.16 + flow * 0.8) * noise(island * 0.1 - flow * 0.5) * 1.9);
    float day = 1.0 - night * 0.85;
    return base * (0.94 + swell * 0.12) + float3(0.75, 0.9, 0.95) * glitter * 0.1 * day;
}
fragment float4 main0(In in [[stage_in]], texture2d<float> t [[texture(0)]], sampler linear [[sampler(0)]], constant Settings& s [[buffer(0)]]) {
    float3 low, high;
    float3 c = catmull(t, in.uv, s.size, low, high);
    c = clamp(c, low, high);
    float2 game = in.uv * s.size;
    bool world = s.active > 0.5;
    for (int i = 0; world && i < s.count; ++i)
        if (all(game >= s.areas[i].xy) && all(game < s.areas[i].zw)) world = false;
    if (world) {
        float3 cleaned;
        if (s.edges != 0 && clean_edge(t, game, cleaned)) c = cleaned;
        float2 kind = ground(t, game);
        float2 island = (game - s.origin) / s.scale;
        if (kind.x > 0.01) {
            float reach = 8.0;
            // Open beach, seen from most directions: buildings over water must not grow foam.
            float near_sand = 0;
            for (int i = 0; i < 8; ++i) {
                float angle = float(i) * 0.7854;
                float2 step = float2(cos(angle), sin(angle) * 0.6);
                if (is_sand(at(t, int2(game + step * reach * s.scale * 0.5)))) near_sand += 1.0;
                if (is_sand(at(t, int2(game + step * reach * s.scale * 1.5)))) near_sand += 1.0;
            }
            near_sand = smoothstep(4.0, 8.0, near_sand);
            float3 water = ocean(t, linear, game, island, s.time, s.night, s.size, s.scale);
            // Foam laps in and out along the shore.
            float lap = 0.5 + 0.5 * sin(s.time * 0.8 + noise(island * 0.02) * 12.0);
            float lace = smoothstep(0.42, 0.7, noise(island * 0.28 + float2(s.time * 0.25, -s.time * 0.15)) + lap * 0.22);
            water = mix(water, float3(0.93, 0.97, 0.96) * (1.0 - s.night * 0.6), near_sand * lace * 0.75);
            c = mix(c, water, kind.x);
        }
        if (kind.y > 0.01) {
            float2 cell = island;
            float grain = (noise(cell * 1.7) - 0.5) * 0.045 + (noise(cell * 0.35) - 0.5) * 0.03;
            float wet = 0;
            float reach = 7.0 * s.scale;
            for (int i = 0; i < 6; ++i) {
                float angle = float(i) * 1.0472;
                if (is_water(at(t, int2(game + float2(cos(angle), sin(angle) * 0.6) * reach)))) wet = 1;
            }
            float lap = 0.72 + 0.28 * sin(s.time * 0.8 + noise(island * 0.02) * 12.0);
            float3 sand = c * (1.0 + grain) * mix(1.0, float3(0.8, 0.8, 0.84), wet * lap);
            c = mix(c, sand, kind.y);
        }
        if (s.fall > 0.5) c = waterfall(t, linear, c, game, island, s.time, s.night, s.size, s.scale);
        return float4(saturate(c), 1.0);
    }
    float2 d = 1.0 / s.size;
    // One filtered read per neighbour: this runs for every pixel of a large display.
    float3 n = t.sample(linear, in.uv + float2(0, -d.y)).rgb;
    float3 so = t.sample(linear, in.uv + float2(0, d.y)).rgb;
    float3 w = t.sample(linear, in.uv + float2(-d.x, 0)).rgb;
    float3 e = t.sample(linear, in.uv + float2(d.x, 0)).rgb;
    float3 mn = min(c, min(n, min(so, min(w, e))));
    float3 mx = max(c, max(n, max(so, max(w, e))));
    float3 amount = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float3 peak = -1.0 / mix(8.0, 5.0, s.sharpness);
    float3 weight = amount * peak;
    float3 r = (c + (n + so + w + e) * weight) / (1.0 + 4.0 * weight);
    return float4(saturate(r), 1.0);
}
)";

// A soft shadow under each person on land, and ripples around them in the water. Each is only
// drawn over ground, so it cannot land on the person or on the interface.
const char people_fragment_source[] = R"(#include <metal_stdlib>
using namespace metal;
struct In { float4 position [[position]]; float2 game; float2 local; float2 uv; float zoom; };
struct P { float time; float night; int count; float pad; float4 areas[8]; };
fragment float4 main0(In in [[stage_in]], texture2d<float> frame [[texture(0)]], constant P& p [[buffer(0)]]) {
    float3 c = frame.read(uint2(clamp(int2(in.game), int2(0), int2(frame.get_width(), frame.get_height()) - 1))).rgb;
    for (int i = 0; i < p.count; ++i)
        if (all(in.game >= p.areas[i].xy) && all(in.game < p.areas[i].zw)) discard_fragment();
    float2 q = (in.uv - 0.5) * 2.0;
    float r = length(q);
    if (r >= 1.0) discard_fragment();
    if (c.b > c.r * 1.2 && c.g > c.r * 1.1 && c.b < c.g * 1.2) {
        float ring = sin(r * 15.0 - p.time * 3.2) * 0.5 + 0.5;
        float a = pow(ring, 5.0) * (1.0 - r) * 0.42 * (1.0 - p.night * 0.6);
        return float4(0.92, 0.97, 1.0, a);
    }
    // Sand and grass only: skin and clothes are warmer than sand, so people stay untouched.
    bool land = c.r > c.b && c.r - c.b > 0.05 && c.g > 0.2 && c.r - c.g < 0.12;
    if (!land) discard_fragment();
    float a = pow(1.0 - r, 0.8) * 0.26 * (1.0 - p.night * 0.7);
    return float4(0.05, 0.04, 0.08, a);
}
)";
// Light over the whole island, applied after the HD art so it all matches: drifting cloud
// shadows, warm sunrise and sunset colour, a gentle vignette. The output multiplies the picture.
const char grade_fragment_source[] = R"(#include <metal_stdlib>
using namespace metal;
struct In { float4 position [[position]]; float2 uv; };
struct G { float2 size; float2 origin; float scale; float time; float night; float warm; float4 areas[8]; int count; int pad[3]; };
static float hash(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
static float noise(float2 p) {
    float2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + float2(1, 0)), f.x), mix(hash(i + float2(0, 1)), hash(i + float2(1, 1)), f.x), f.y);
}
fragment float4 main0(In in [[stage_in]], constant G& g [[buffer(0)]]) {
    float2 game = in.uv * g.size;
    for (int i = 0; i < g.count; ++i)
        if (all(game >= g.areas[i].xy) && all(game < g.areas[i].zw)) discard_fragment();
    float2 island = (game - g.origin) / g.scale;
    float2 drift = float2(g.time * 4.0, g.time * 1.6);
    float cloud = noise((island + drift) * 0.004) * 0.7 + noise((island + drift) * 0.011) * 0.3;
    float shade = 1.0 - smoothstep(0.52, 0.72, cloud) * 0.08 * (1.0 - g.night);
    float2 centre = in.uv - 0.5;
    float vignette = 1.0 - smoothstep(0.42, 0.8, length(centre)) * 0.08;
    float3 tint = mix(float3(1.0), float3(1.0, 0.86, 0.68), g.warm);
    return float4(tint * shade * vignette, 1.0);
}
)";
// A soft glow around the brightest things once it gets dark: lit windows, neon, the casino.
const char glow_fragment_source[] = R"(#include <metal_stdlib>
using namespace metal;
struct In { float4 position [[position]]; float2 uv; };
struct G { float2 size; float2 origin; float scale; float time; float night; float warm; float4 areas[8]; int count; int pad[3]; };
fragment float4 main0(In in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant G& g [[buffer(0)]]) {
    float2 game = in.uv * g.size;
    for (int i = 0; i < g.count; ++i)
        if (all(game >= g.areas[i].xy) && all(game < g.areas[i].zw)) discard_fragment();
    float3 sum = 0;
    float2 d = 1.0 / g.size;
    for (int ring = 1; ring <= 2; ++ring)
        for (int i = 0; i < 8; ++i) {
            float angle = float(i) * 0.7854 + float(ring) * 0.39;
            float3 c = t.sample(s, in.uv + float2(cos(angle), sin(angle)) * d * float(ring) * 3.5).rgb;
            float level = max(c.r, max(c.g, c.b));
            sum += c * smoothstep(0.55, 0.85, level) / float(ring);
        }
    return float4(sum / 12.0 * g.night * 0.55, 1.0);
}
)";

// The HD overlay draws each visible building's HD sprite over the picture, but only where
// the game's own frame still shows the original sprite, up to a lighting tint. Whatever
// differs there (a guest in front, the interface) keeps the game's pixel.
const char hd_vertex_source[] = R"(#include <metal_stdlib>
using namespace metal;
struct Settings { float2 frame; float2 origin; float2 size; float2 zoom; };
struct Out { float4 position [[position]]; float2 game; float2 local; float2 uv; float zoom; };
vertex Out main0(uint id [[vertex_id]], constant Settings& s [[buffer(0)]]) {
    float2 uv = float2((id & 1), (id >> 1) & 1);
    Out out;
    out.uv = uv;
    out.local = uv * s.size;
    out.game = s.origin + out.local * s.zoom.x;
    out.zoom = s.zoom.x;
    float2 p = out.game / s.frame;
    out.position = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    return out;
}
)";
const char hd_fragment_source[] = R"(#include <metal_stdlib>
using namespace metal;
struct In { float4 position [[position]]; float2 game; float2 local; float2 uv; float zoom; };
struct Mask { float4 areas[8]; int count; int kind; float time; float pad; };
// Whether the game's pixel still shows the original sprite, up to a lighting tint.
static bool shows(float3 shown, float3 original, thread float3& tint, thread bool& same) {
    tint = (shown + 0.02) / (original + 0.02);
    float spread = max(tint.r, max(tint.g, tint.b)) - min(tint.r, min(tint.g, tint.b));
    float level = (tint.r + tint.g + tint.b) / 3.0;
    // Ratios are unstable for dark colours, so near-identical pixels always match.
    float3 difference = abs(shown - original);
    same = max(difference.r, max(difference.g, difference.b)) < 0.05;
    return same || !(spread > 0.25 || level < 0.2 || level > 1.6);
}
fragment float4 main0(In in [[stage_in]], texture2d<float> frame [[texture(0)]],
    texture2d<float> lo [[texture(1)]], texture2d<float> hd [[texture(2)]],
    sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]], sampler s2 [[sampler(2)]],
    constant Mask& mask [[buffer(0)]]) {
    for (int i = 0; mask.kind != 2 && i < mask.count; ++i)
        if (all(in.game >= mask.areas[i].xy) && all(in.game < mask.areas[i].zw)) discard_fragment();
    int2 local = int2(in.local);
    int2 game = int2(in.game);
    int2 last = int2(lo.get_width(), lo.get_height()) - 1;
    int2 frame_last = int2(frame.get_width(), frame.get_height()) - 1;
    if (lo.read(uint2(local)).a < 0.5) discard_fragment();
    // A lone match is chance, and a lone miss is noise: let the neighbours vote.
    int total = 0, matches = 0;
    float3 tint = 1;
    bool same = true;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            int2 at = clamp(int2(in.local + float2(x, y) / in.zoom), int2(0), last);
            float4 original = lo.read(uint2(at));
            if (original.a < 0.5) continue;
            float3 shown = frame.read(uint2(clamp(game + int2(x, y), int2(0), frame_last))).rgb;
            float3 t;
            bool s;
            ++total;
            if (shows(shown, original.rgb, t, s)) ++matches;
            if (x == 0 && y == 0) { tint = t; same = s; }
        }
    if (matches * 4 < total * 3) discard_fragment();
    float2 uv = in.uv;
    float4 art = hd.sample(s2, uv);
    float sparkle = 0;
    if (mask.kind == 1) {
        // Life for the buildings, found from the art's colours: plants stir, and water ripples.
        float2 texel = 1.0 / float2(lo.get_width(), lo.get_height());
        if (art.g > art.r * 1.12 && art.g > art.b * 1.12) {
            float phase = mask.time * 1.4 + (in.uv.x + in.uv.y) * 40.0;
            uv += float2(sin(phase), cos(phase * 0.7) * 0.5) * texel * 0.5;
            art = hd.sample(s2, uv);
        } else if (art.b > art.r * 1.3 && art.b > art.g * 1.02 && art.b > 0.35) {
            float2 wave = float2(sin(mask.time * 1.7 + in.uv.y * 90.0), cos(mask.time * 1.3 + in.uv.x * 80.0));
            uv += wave * texel * 0.45;
            art = hd.sample(s2, uv);
            sparkle = pow(saturate(0.5 + 0.5 * sin(mask.time * 2.3 + in.uv.x * 170.0 + in.uv.y * 130.0 + wave.x * 2.0)), 24.0) * 0.28;
        }
    }
    float3 applied = same ? float3(1) : clamp(tint, 0.2, 1.6);
    return float4(saturate(art.rgb * applied + sparkle), art.a);
}
)";

struct HdItem
{
    std::string key;
    int x, y;
    float scale;
    int kind; // 1 building, 2 interface, 3 animated overlay
};
std::vector<HdItem> hd_items;
std::vector<std::array<float, 4>> hd_excluded;
std::vector<HdGlyph> hd_glyphs_list;
struct HdWorld
{
    bool active = false;
    float origin_x = 0, origin_y = 0, scale = 1, hour = 12;
    bool lighting = false;
};
HdWorld hd_world;
struct HdPerson
{
    int x, y;
    float scale;
};
std::vector<HdPerson> hd_people;
bool hd_waterfall = false;
int hd_text_enabled = 0;
int hd_text_paused = 0;

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message + ": " + SDL_GetError());
}

const char* driver_name(GraphicsApi api)
{
    switch (api)
    {
    case GraphicsApi::Direct3D12:
        return "direct3d12";
    case GraphicsApi::Vulkan:
        return "vulkan";
    case GraphicsApi::Metal:
        return "metal";
    case GraphicsApi::Automatic:
        break;
    }
    return nullptr;
}

const char* api_title(GraphicsApi api)
{
    switch (api)
    {
    case GraphicsApi::Direct3D12:
        return "Direct3D 12";
    case GraphicsApi::Vulkan:
        return "Vulkan";
    case GraphicsApi::Metal:
        return "Metal";
    case GraphicsApi::Automatic:
        break;
    }
    return "A supported graphics API";
}

class SdlGpuVideo : public Video
{
  public:
    explicit SdlGpuVideo(const VideoSettings& settings)
        : settings_(settings)
    {
        // Opened hidden, placed on its display, then shown.
        window_ = SDL_CreateWindow(settings.title.c_str(),
            settings.width,
            settings.height,
            SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_)
        {
            fail("Cannot open the game window");
        }
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        const SDL_DisplayID display = displays && settings.display > 0 && settings.display <= count
                                          ? displays[settings.display - 1]
                                          : SDL_GetPrimaryDisplay();
        SDL_free(displays);
        SDL_SetWindowPosition(window_,
            SDL_WINDOWPOS_CENTERED_DISPLAY(display),
            SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        if (settings.fullscreen)
        {
            SDL_SetWindowFullscreenMode(
                window_, nullptr); // the display's own mode, without a mode change
            SDL_SetWindowFullscreen(window_, true);
        }

        // The overlay's shaders come as SPIR-V, DXBC and metallib; every backend takes one of them.
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_METALLIB |
                SDL_GPU_SHADERFORMAT_MSL, // the iOS simulator's overlay compiles Metal source
            false,
            driver_name(settings.api));
        if (!device_)
        {
            fail(std::string(api_title(settings.api)) +
                 " is not available on this computer. Choose another graphics "
                 "API on the Display page");
        }
        if (!SDL_ClaimWindowForGPUDevice(device_, window_))
        {
            fail("Cannot draw into the game window");
        }
        claimed_ = true;
        info_.api = SDL_GetGPUDeviceDriver(device_);

        // Claiming the window sets up SDR with VSync. Without VSync, the fastest mode the
        // window supports; where that cannot be set, VSync stays (and is reported).
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (!settings.vsync)
        {
            if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE))
            {
                mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            }
            else if (SDL_WindowSupportsGPUPresentMode(
                         device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
            {
                mode = SDL_GPU_PRESENTMODE_MAILBOX;
            }
            if (mode != SDL_GPU_PRESENTMODE_VSYNC &&
                !SDL_SetGPUSwapchainParameters(
                    device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            {
                mode = SDL_GPU_PRESENTMODE_VSYNC;
            }
        }
        info_.present_mode = mode == SDL_GPU_PRESENTMODE_VSYNC     ? "vsync"
                             : mode == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox"
                                                                   : "immediate";
        swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(device_, window_);

        ImGui_ImplSDLGPU3_InitInfo overlay{};
        overlay.Device = device_;
        overlay.ColorTargetFormat = swapchain_format_;
        if (!ImGui_ImplSDLGPU3_Init(&overlay))
        {
            throw std::runtime_error("Cannot set up the overlay renderer");
        }
        overlay_ready_ = true;
        create_scaler();
        create_hd();
        create_fx();
        if (!settings.hidden)
        {
            SDL_ShowWindow(window_);
        }
    }

    ~SdlGpuVideo() override
    {
        if (device_)
        {
            SDL_WaitForGPUIdle(device_);
        }
        if (overlay_ready_)
        {
            ImGui_ImplSDLGPU3_Shutdown();
        }
        if (device_ && scaler_)
        {
            SDL_ReleaseGPUGraphicsPipeline(device_, scaler_);
        }
        for (SDL_GPUGraphicsPipeline* pipeline : {people_pipeline_, grade_pipeline_, glow_pipeline_})
        {
            if (device_ && pipeline)
            {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        if (device_ && hd_pipeline_)
        {
            SDL_ReleaseGPUGraphicsPipeline(device_, hd_pipeline_);
        }
        if (device_)
        {
            for (const auto& [name, art] : hd_art_)
            {
                if (art.hd)
                {
                    SDL_ReleaseGPUTexture(device_, art.hd);
                    SDL_ReleaseGPUTexture(device_, art.lo);
                }
            }
        }
        if (device_ && scaler_sampler_)
        {
            SDL_ReleaseGPUSampler(device_, scaler_sampler_);
        }
        if (device_)
        {
            for (auto* texture : {frame_texture_, composite_})
            {
                if (texture)
                {
                    SDL_ReleaseGPUTexture(device_, texture);
                }
            }
            for (auto* buffer : {upload_, download_})
            {
                if (buffer)
                {
                    SDL_ReleaseGPUTransferBuffer(device_, buffer);
                }
            }
            if (claimed_)
            {
                SDL_ReleaseWindowFromGPUDevice(device_, window_);
            }
            SDL_DestroyGPUDevice(device_);
        }
        if (window_)
        {
            SDL_DestroyWindow(window_);
        }
    }

    SDL_Window* window() const override
    {
        return window_;
    }
    const VideoInfo& info() const override
    {
        return info_;
    }
    bool stretch() const override
    {
        return settings_.stretch;
    }
    void new_overlay_frame() override
    {
        // Fonts added since the atlas was uploaded: upload it again.
        if (!ImGui::GetIO().Fonts->IsBuilt())
        {
            ImGui_ImplSDLGPU3_DestroyFontsTexture();
        }
        ImGui_ImplSDLGPU3_NewFrame();
    }
    void save_next(const std::filesystem::path& file) override
    {
        screenshot_ = file;
    }

    void present(const Frame& frame, ImDrawData* overlay) override
    {
        if (!overlay)
        {
            overlay = host_overlay();
        }
        const auto width = static_cast<Uint32>(frame.width),
                   height = static_cast<Uint32>(frame.height);
        if (frame.pixels.size() != static_cast<std::size_t>(width) * height)
        {
            throw std::runtime_error("Malformed frame");
        }
        set_frame_size(frame.width, frame.height);
        prepare_frame(width, height);
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (!commands)
        {
            fail("Cannot record GPU commands");
        }

        auto* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
        if (!mapped)
        {
            fail("Cannot upload the frame");
        }
        std::memcpy(mapped, frame.pixels.data(), frame.pixels.size() * 4);
        SDL_UnmapGPUTransferBuffer(device_, upload_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureTransferInfo source{upload_, 0, width, height};
        SDL_GPUTextureRegion destination{};
        destination.texture = frame_texture_;
        destination.w = width;
        destination.h = height;
        destination.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &destination, true);
        SDL_EndGPUCopyPass(copy);
        if (overlay)
        {
            Imgui_ImplSDLGPU3_PrepareDrawData(overlay, commands);
        }

        prepare_hd(commands);
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 window_width = 0, window_height = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(
                commands, window_, &swapchain, &window_width, &window_height))
        {
            fail("Cannot present the frame");
        }
        if (!swapchain)
        { // minimized: nothing to show
            SDL_SubmitGPUCommandBuffer(commands);
            return;
        }
        const bool capture = !screenshot_.empty();
        SDL_GPUTexture* target =
            capture ? prepare_composite(window_width, window_height) : swapchain;

        // Black bars, then the frame scaled into place, then the overlay.
        SDL_GPUColorTargetInfo clear{};
        clear.texture = target;
        clear.clear_color = {0, 0, 0, 1};
        clear.load_op = SDL_GPU_LOADOP_CLEAR;
        clear.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &clear, 1, nullptr));
        const SDL_Rect rect =
            presentation_transform(static_cast<int>(window_width), static_cast<int>(window_height))
                .destination;
        if (scaler_)
        {
            SDL_GPUColorTargetInfo color{};
            color.texture = target;
            color.load_op = SDL_GPU_LOADOP_LOAD;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, scaler_);
            const SDL_GPUViewport viewport{static_cast<float>(rect.x),
                static_cast<float>(rect.y),
                static_cast<float>(rect.w),
                static_cast<float>(rect.h),
                0,
                1};
            SDL_SetGPUViewport(pass, &viewport);
            const SDL_GPUTextureSamplerBinding binding{frame_texture_, scaler_sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            const WorldLight light = world_light();
            struct
            {
                float size[2];
                float sharpness, active;
                float origin[2];
                float scale, time;
                float areas[8][4];
                int count, edges;
                float night, fall;
            } settings{};
            settings.size[0] = static_cast<float>(width);
            settings.size[1] = static_cast<float>(height);
            settings.sharpness = scaler_sharpness_;
            settings.active = fx_enabled_ && hd_world.active ? 1.0f : 0.0f;
            settings.origin[0] = hd_world.origin_x * hd_world.scale;
            settings.origin[1] = hd_world.origin_y * hd_world.scale;
            settings.scale = hd_world.scale;
            settings.time = static_cast<float>(SDL_GetTicks()) / 1000.0f;
            for (const auto& area : hd_excluded)
            {
                if (settings.count < 8)
                {
                    std::copy(area.begin(), area.end(), settings.areas[settings.count++]);
                }
            }
            settings.edges = edges_enabled_ ? 1 : 0;
            settings.night = light.night;
            settings.fall = hd_waterfall ? 1.0f : 0.0f;
            SDL_PushGPUFragmentUniformData(commands, 0, &settings, sizeof settings);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
        else
        {
            SDL_GPUBlitInfo blit{};
            blit.source = {frame_texture_, 0, 0, 0, 0, width, height};
            blit.destination = {target,
                0,
                0,
                static_cast<Uint32>(rect.x),
                static_cast<Uint32>(rect.y),
                static_cast<Uint32>(rect.w),
                static_cast<Uint32>(rect.h)};
            blit.load_op = SDL_GPU_LOADOP_LOAD;
            blit.filter = SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(commands, &blit);
        }
        draw_people(commands, target, rect, width, height);
        draw_hd(commands, target, rect, width, height);
        draw_grade(commands, target, rect, width, height);
        if (overlay)
        {
            SDL_GPUColorTargetInfo layer{};
            layer.texture = target;
            layer.load_op = SDL_GPU_LOADOP_LOAD;
            layer.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &layer, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(overlay, commands, pass);
            SDL_EndGPURenderPass(pass);
        }
        if (!capture)
        {
            if (!SDL_SubmitGPUCommandBuffer(commands))
            {
                fail("Cannot submit the frame");
            }
            return;
        }
        save_composite(commands, swapchain, window_width, window_height);
    }

  private:
    // Falls back to the bilinear blit whenever the sharper path is unavailable.
    void create_scaler()
    {
        const char* choice = SDL_getenv("BEACHLIFE_SCALER");
        if (std::strcmp(SDL_GetGPUDeviceDriver(device_), "metal") != 0 ||
            (choice && std::strcmp(choice, "linear") == 0))
        {
            return;
        }
        if (const char* amount = SDL_getenv("BEACHLIFE_SCALER_SHARPNESS"))
        {
            scaler_sharpness_ = static_cast<float>(SDL_atof(amount));
        }
        SDL_GPUShaderCreateInfo vertex{};
        vertex.code = reinterpret_cast<const Uint8*>(scaler_source);
        vertex.code_size = sizeof scaler_source - 1;
        vertex.entrypoint = "main0";
        vertex.format = SDL_GPU_SHADERFORMAT_MSL;
        vertex.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragment{};
        fragment.code = reinterpret_cast<const Uint8*>(scaler_fragment_source);
        fragment.code_size = sizeof scaler_fragment_source - 1;
        fragment.entrypoint = "main0";
        fragment.format = SDL_GPU_SHADERFORMAT_MSL;
        fragment.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragment.num_samplers = 1;
        fragment.num_uniform_buffers = 1;
        SDL_GPUShader* vs = SDL_CreateGPUShader(device_, &vertex);
        SDL_GPUShader* fs = SDL_CreateGPUShader(device_, &fragment);
        SDL_GPUSamplerCreateInfo sampler{};
        sampler.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
        sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        scaler_sampler_ = SDL_CreateGPUSampler(device_, &sampler);
        if (vs && fs && scaler_sampler_)
        {
            SDL_GPUColorTargetDescription description{};
            description.format = swapchain_format_;
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vs;
            info.fragment_shader = fs;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.target_info.color_target_descriptions = &description;
            info.target_info.num_color_targets = 1;
            scaler_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        }
        if (!scaler_)
        {
            std::fprintf(stderr, "Sharper scaling unavailable: %s\n", SDL_GetError());
        }
        if (vs)
        {
            SDL_ReleaseGPUShader(device_, vs);
        }
        if (fs)
        {
            SDL_ReleaseGPUShader(device_, fs);
        }
    }

    struct WorldLight
    {
        float night = 0, warm = 0;
    };

    // From the game's clock: dark from 21:00 to 05:00, with warm light around dawn and dusk.
    static WorldLight world_light()
    {
        WorldLight light;
        if (!hd_world.active || (!hd_world.lighting && !SDL_getenv("BEACHLIFE_FX_HOUR")))
        {
            return light;
        }
        const auto ramp = [](float edge0, float edge1, float x) {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3 - 2 * t);
        };
        // BEACHLIFE_FX_HOUR previews the light at a fixed time of day.
        const char* preview = SDL_getenv("BEACHLIFE_FX_HOUR");
        const float hour = preview ? static_cast<float>(SDL_atof(preview)) : hd_world.hour;
        light.night = std::max(ramp(18.5f, 21.0f, hour), 1 - ramp(5.0f, 7.0f, hour));
        light.warm = std::max(ramp(16.5f, 18.5f, hour) * (1 - ramp(19.5f, 21.0f, hour)),
                         ramp(5.0f, 6.0f, hour) * (1 - ramp(6.5f, 8.0f, hour))) *
                     0.8f;
        return light;
    }

    SDL_GPUGraphicsPipeline* make_pipeline(const char* vertex_source, std::size_t vertex_size,
        const char* fragment_source, std::size_t fragment_size, Uint32 samplers,
        Uint32 vertex_uniforms, SDL_GPUPrimitiveType primitive,
        const SDL_GPUColorTargetBlendState& blend)
    {
        SDL_GPUShaderCreateInfo vertex{};
        vertex.code = reinterpret_cast<const Uint8*>(vertex_source);
        vertex.code_size = vertex_size;
        vertex.entrypoint = "main0";
        vertex.format = SDL_GPU_SHADERFORMAT_MSL;
        vertex.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vertex.num_uniform_buffers = vertex_uniforms;
        SDL_GPUShaderCreateInfo fragment{};
        fragment.code = reinterpret_cast<const Uint8*>(fragment_source);
        fragment.code_size = fragment_size;
        fragment.entrypoint = "main0";
        fragment.format = SDL_GPU_SHADERFORMAT_MSL;
        fragment.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragment.num_samplers = samplers;
        fragment.num_uniform_buffers = 1;
        SDL_GPUShader* vs = SDL_CreateGPUShader(device_, &vertex);
        SDL_GPUShader* fs = SDL_CreateGPUShader(device_, &fragment);
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        if (vs && fs)
        {
            SDL_GPUColorTargetDescription description{};
            description.format = swapchain_format_;
            description.blend_state = blend;
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vs;
            info.fragment_shader = fs;
            info.primitive_type = primitive;
            info.target_info.color_target_descriptions = &description;
            info.target_info.num_color_targets = 1;
            pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        }
        if (!pipeline)
        {
            std::fprintf(stderr, "A world effect is unavailable: %s\n", SDL_GetError());
        }
        if (vs)
        {
            SDL_ReleaseGPUShader(device_, vs);
        }
        if (fs)
        {
            SDL_ReleaseGPUShader(device_, fs);
        }
        return pipeline;
    }

    void create_fx()
    {
        const auto off = [](const char* name) {
            const char* value = SDL_getenv(name);
            return value && std::strcmp(value, "0") == 0;
        };
        edges_enabled_ = !off("BEACHLIFE_EDGES");
        if (!scaler_ || off("BEACHLIFE_FX"))
        {
            edges_enabled_ = false;
            return;
        }
        fx_enabled_ = true;
        SDL_GPUColorTargetBlendState alpha{};
        alpha.enable_blend = true;
        alpha.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        alpha.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        alpha.color_blend_op = SDL_GPU_BLENDOP_ADD;
        alpha.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        alpha.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        alpha.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        people_pipeline_ = make_pipeline(hd_vertex_source, sizeof hd_vertex_source - 1,
            people_fragment_source, sizeof people_fragment_source - 1, 1, 1,
            SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP, alpha);
        SDL_GPUColorTargetBlendState multiply = alpha;
        multiply.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        multiply.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        grade_pipeline_ = make_pipeline(scaler_source, sizeof scaler_source - 1, grade_fragment_source,
            sizeof grade_fragment_source - 1, 0, 0, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, multiply);
        SDL_GPUColorTargetBlendState add = alpha;
        add.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        add.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        glow_pipeline_ = make_pipeline(scaler_source, sizeof scaler_source - 1, glow_fragment_source,
            sizeof glow_fragment_source - 1, 1, 0, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, add);
    }

    void draw_people(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, const SDL_Rect& rect,
        Uint32 width, Uint32 height)
    {
        if (!people_pipeline_ || !hd_world.active || hd_people.empty())
        {
            return;
        }
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, people_pipeline_);
        const SDL_GPUViewport viewport{static_cast<float>(rect.x),
            static_cast<float>(rect.y),
            static_cast<float>(rect.w),
            static_cast<float>(rect.h),
            0,
            1};
        SDL_SetGPUViewport(pass, &viewport);
        // Stays out of the interface, which the shader cannot tell from sand.
        float bottom = static_cast<float>(height);
        for (const auto& area : hd_excluded)
        {
            if (area[0] <= 0 && area[2] >= static_cast<float>(width))
            {
                bottom = std::min(bottom, area[1]);
            }
        }
        const SDL_GPUTextureSamplerBinding binding{frame_texture_, scaler_sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        struct
        {
            float time, night;
            int count;
            float pad;
            float areas[8][4];
        } fragment{};
        fragment.time = static_cast<float>(SDL_GetTicks()) / 1000.0f;
        fragment.night = world_light().night;
        for (const auto& area : hd_excluded)
        {
            if (fragment.count < 8)
            {
                std::copy(area.begin(), area.end(), fragment.areas[fragment.count++]);
            }
        }
        SDL_PushGPUFragmentUniformData(commands, 0, &fragment, sizeof fragment);
        constexpr float spread_x = 28, spread_y = 13;
        for (const auto& person : hd_people)
        {
            if (static_cast<float>(person.y) >= bottom)
            {
                continue;
            }
            const float x = static_cast<float>(person.x) - spread_x * person.scale * 0.5f;
            const float y = static_cast<float>(person.y) - spread_y * person.scale * 0.5f;
            const float uniforms[8] = {static_cast<float>(width),
                static_cast<float>(height),
                x,
                y,
                spread_x,
                spread_y,
                person.scale,
                0};
            SDL_PushGPUVertexUniformData(commands, 0, uniforms, sizeof uniforms);
            SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }

    void draw_grade(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, const SDL_Rect& rect,
        Uint32 width, Uint32 height)
    {
        if (!grade_pipeline_ || !hd_world.active)
        {
            return;
        }
        const WorldLight light = world_light();
        struct
        {
            float size[2];
            float origin[2];
            float scale, time, night, warm;
            float areas[8][4];
            int count;
            int pad[3];
        } settings{};
        settings.size[0] = static_cast<float>(width);
        settings.size[1] = static_cast<float>(height);
        settings.origin[0] = hd_world.origin_x * hd_world.scale;
        settings.origin[1] = hd_world.origin_y * hd_world.scale;
        settings.scale = hd_world.scale;
        settings.time = static_cast<float>(SDL_GetTicks()) / 1000.0f;
        settings.night = light.night;
        settings.warm = light.warm;
        for (const auto& area : hd_excluded)
        {
            if (settings.count < 8)
            {
                std::copy(area.begin(), area.end(), settings.areas[settings.count++]);
            }
        }
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
        const SDL_GPUViewport viewport{static_cast<float>(rect.x),
            static_cast<float>(rect.y),
            static_cast<float>(rect.w),
            static_cast<float>(rect.h),
            0,
            1};
        SDL_SetGPUViewport(pass, &viewport);
        SDL_BindGPUGraphicsPipeline(pass, grade_pipeline_);
        SDL_PushGPUFragmentUniformData(commands, 0, &settings, sizeof settings);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        if (glow_pipeline_ && light.night > 0.01f)
        {
            SDL_BindGPUGraphicsPipeline(pass, glow_pipeline_);
            const SDL_GPUTextureSamplerBinding binding{frame_texture_, scaler_sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(commands, 0, &settings, sizeof settings);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }

    struct HdArt
    {
        SDL_GPUTexture* hd = nullptr;
        SDL_GPUTexture* lo = nullptr;
        float width = 0, height = 0;
    };

    void create_hd()
    {
        const char* choice = SDL_getenv("BEACHLIFE_HD");
        const char* base = SDL_GetBasePath();
        if (std::strcmp(SDL_GetGPUDeviceDriver(device_), "metal") != 0 || !scaler_sampler_ ||
            (choice && std::strcmp(choice, "0") == 0) || !base)
        {
            return;
        }
        hd_folder_ = std::string(base) + "hd/";
        std::error_code error;
        if (!std::filesystem::is_directory(utf8_path(hd_folder_), error))
        {
            return;
        }
        SDL_GPUShaderCreateInfo vertex{};
        vertex.code = reinterpret_cast<const Uint8*>(hd_vertex_source);
        vertex.code_size = sizeof hd_vertex_source - 1;
        vertex.entrypoint = "main0";
        vertex.format = SDL_GPU_SHADERFORMAT_MSL;
        vertex.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vertex.num_uniform_buffers = 1;
        SDL_GPUShaderCreateInfo fragment{};
        fragment.code = reinterpret_cast<const Uint8*>(hd_fragment_source);
        fragment.code_size = sizeof hd_fragment_source - 1;
        fragment.entrypoint = "main0";
        fragment.format = SDL_GPU_SHADERFORMAT_MSL;
        fragment.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragment.num_samplers = 3;
        fragment.num_uniform_buffers = 1;
        SDL_GPUShader* vs = SDL_CreateGPUShader(device_, &vertex);
        SDL_GPUShader* fs = SDL_CreateGPUShader(device_, &fragment);
        if (vs && fs)
        {
            SDL_GPUColorTargetDescription description{};
            description.format = swapchain_format_;
            description.blend_state.enable_blend = true;
            description.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            description.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            description.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            description.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            description.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            description.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vs;
            info.fragment_shader = fs;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
            info.target_info.color_target_descriptions = &description;
            info.target_info.num_color_targets = 1;
            hd_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        }
        if (!hd_pipeline_)
        {
            std::fprintf(stderr, "HD art unavailable: %s\n", SDL_GetError());
        }
        if (vs)
        {
            SDL_ReleaseGPUShader(device_, vs);
        }
        if (fs)
        {
            SDL_ReleaseGPUShader(device_, fs);
        }
    }

    SDL_GPUTexture* upload_png(SDL_GPUCopyPass* copy, const std::string& path, float* w, float* h)
    {
        SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
        SDL_Surface* surface = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
        SDL_DestroySurface(loaded);
        if (!surface)
        {
            return nullptr;
        }
        const auto width = static_cast<Uint32>(surface->w), height = static_cast<Uint32>(surface->h);
        SDL_GPUTextureCreateInfo texture{};
        texture.type = SDL_GPU_TEXTURETYPE_2D;
        texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texture.width = width;
        texture.height = height;
        texture.layer_count_or_depth = 1;
        texture.num_levels = 1;
        SDL_GPUTexture* result = SDL_CreateGPUTexture(device_, &texture);
        const SDL_GPUTransferBufferCreateInfo buffer{
            SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, width * height * 4, 0};
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &buffer);
        if (result && transfer)
        {
            auto* mapped = static_cast<unsigned char*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
            for (Uint32 y = 0; mapped && y < height; ++y)
            {
                std::memcpy(mapped + y * width * 4,
                    static_cast<unsigned char*>(surface->pixels) + y * surface->pitch, width * 4);
            }
            SDL_UnmapGPUTransferBuffer(device_, transfer);
            const SDL_GPUTextureTransferInfo source{transfer, 0, width, height};
            SDL_GPUTextureRegion destination{};
            destination.texture = result;
            destination.w = width;
            destination.h = height;
            destination.d = 1;
            SDL_UploadToGPUTexture(copy, &source, &destination, false);
        }
        if (transfer)
        {
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
        }
        SDL_DestroySurface(surface);
        if (w)
        {
            *w = static_cast<float>(width);
            *h = static_cast<float>(height);
        }
        return result;
    }

    // A building's art is loaded the first time it appears. A missing file is remembered.
    void prepare_hd(SDL_GPUCommandBuffer* commands)
    {
        if (!hd_pipeline_)
        {
            return;
        }
        SDL_GPUCopyPass* copy = nullptr;
        for (const auto& item : hd_items)
        {
            const std::string& key = item.key;
            if (hd_art_.contains(key))
            {
                continue;
            }
            if (!copy)
            {
                copy = SDL_BeginGPUCopyPass(commands);
            }
            HdArt art;
            art.hd = upload_png(copy, hd_folder_ + key + ".png", nullptr, nullptr);
            if (art.hd)
            {
                art.lo = upload_png(copy, hd_folder_ + key + "_lo.png", &art.width, &art.height);
            }
            if (art.hd && !art.lo)
            {
                SDL_ReleaseGPUTexture(device_, art.hd);
                art.hd = nullptr;
            }
            if (!art.hd && SDL_getenv("BEACHLIFE_HD_LOG"))
            {
                std::fprintf(stderr, "HD: no art for %s\n", key.c_str());
            }
            hd_art_.emplace(key, art);
            if (art.hd && item.kind == 3)
            {
                hd_animation_order_.push_back(key);
            }
        }
        if (copy)
        {
            SDL_EndGPUCopyPass(copy);
        }
        // Animated overlays have thousands of frames: keep only the most recently loaded.
        while (hd_animation_order_.size() > 256)
        {
            const auto oldest = hd_art_.find(hd_animation_order_.front());
            hd_animation_order_.pop_front();
            if (oldest != hd_art_.end())
            {
                SDL_ReleaseGPUTexture(device_, oldest->second.hd);
                SDL_ReleaseGPUTexture(device_, oldest->second.lo);
                hd_art_.erase(oldest);
            }
        }
    }

    void draw_hd(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, const SDL_Rect& rect,
        Uint32 width, Uint32 height)
    {
        if (!hd_pipeline_ || hd_items.empty())
        {
            return;
        }
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, hd_pipeline_);
        const SDL_GPUViewport viewport{static_cast<float>(rect.x),
            static_cast<float>(rect.y),
            static_cast<float>(rect.w),
            static_cast<float>(rect.h),
            0,
            1};
        SDL_SetGPUViewport(pass, &viewport);
        struct
        {
            float areas[8][4];
            int count;
            int kind;
            float time;
            float pad;
        } mask{};
        for (const auto& area : hd_excluded)
        {
            if (mask.count < 8)
            {
                std::copy(area.begin(), area.end(), mask.areas[mask.count++]);
            }
        }
        mask.time = static_cast<float>(SDL_GetTicks()) / 1000.0f;
        const char* effects = SDL_getenv("BEACHLIFE_HD_EFFECTS");
        const bool effects_on = !effects || std::strcmp(effects, "0") != 0;
        // The world first: the interface was drawn over it.
        std::stable_sort(hd_items.begin(), hd_items.end(),
            [](const HdItem& a, const HdItem& b) { return (a.kind == 2) < (b.kind == 2); });
        for (const auto& item : hd_items)
        {
            const auto found = hd_art_.find(item.key);
            if (found == hd_art_.end() || !found->second.hd)
            {
                continue;
            }
            const HdArt& art = found->second;
            const SDL_GPUTextureSamplerBinding bindings[3] = {{frame_texture_, scaler_sampler_},
                {art.lo, scaler_sampler_},
                {art.hd, scaler_sampler_}};
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
            mask.kind = item.kind == 1 && !effects_on ? 4 : item.kind;
            SDL_PushGPUFragmentUniformData(commands, 0, &mask, sizeof mask);
            const float uniforms[8] = {static_cast<float>(width),
                static_cast<float>(height),
                static_cast<float>(item.x),
                static_cast<float>(item.y),
                art.width,
                art.height,
                item.scale,
                0};
            SDL_PushGPUVertexUniformData(commands, 0, uniforms, sizeof uniforms);
            SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }

    void prepare_frame(Uint32 width, Uint32 height)
    {
        if (frame_texture_ && frame_width_ == width && frame_height_ == height)
        {
            return;
        }
        SDL_WaitForGPUIdle(device_);
        if (frame_texture_)
        {
            SDL_ReleaseGPUTexture(device_, frame_texture_);
        }
        if (upload_)
        {
            SDL_ReleaseGPUTransferBuffer(device_, upload_);
        }
        SDL_GPUTextureCreateInfo texture{};
        texture.type = SDL_GPU_TEXTURETYPE_2D;
        texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texture.width = width;
        texture.height = height;
        texture.layer_count_or_depth = 1;
        texture.num_levels = 1;
        frame_texture_ = SDL_CreateGPUTexture(device_, &texture);
        const SDL_GPUTransferBufferCreateInfo buffer{
            SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, width * height * 4, 0};
        upload_ = SDL_CreateGPUTransferBuffer(device_, &buffer);
        if (!frame_texture_ || !upload_)
        {
            fail("Cannot create the frame texture");
        }
        frame_width_ = width;
        frame_height_ = height;
    }

    SDL_GPUTexture* prepare_composite(Uint32 width, Uint32 height)
    {
        if (composite_ && composite_width_ == width && composite_height_ == height)
        {
            return composite_;
        }
        SDL_WaitForGPUIdle(device_);
        if (composite_)
        {
            SDL_ReleaseGPUTexture(device_, composite_);
        }
        if (download_)
        {
            SDL_ReleaseGPUTransferBuffer(device_, download_);
        }
        SDL_GPUTextureCreateInfo texture{};
        texture.type = SDL_GPU_TEXTURETYPE_2D;
        texture.format = swapchain_format_;
        texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texture.width = width;
        texture.height = height;
        texture.layer_count_or_depth = 1;
        texture.num_levels = 1;
        composite_ = SDL_CreateGPUTexture(device_, &texture);
        const SDL_GPUTransferBufferCreateInfo buffer{
            SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, width * height * 4, 0};
        download_ = SDL_CreateGPUTransferBuffer(device_, &buffer);
        if (!composite_ || !download_)
        {
            fail("Cannot prepare the screenshot");
        }
        composite_width_ = width;
        composite_height_ = height;
        return composite_;
    }

    // The picture was composed off screen: show it, and keep a copy as PNG.
    void save_composite(
        SDL_GPUCommandBuffer* commands, SDL_GPUTexture* swapchain, Uint32 width, Uint32 height)
    {
        SDL_GPUBlitInfo show{};
        show.source = {composite_, 0, 0, 0, 0, width, height};
        show.destination = {swapchain, 0, 0, 0, 0, width, height};
        show.load_op = SDL_GPU_LOADOP_DONT_CARE;
        show.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &show);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion source{};
        source.texture = composite_;
        source.w = width;
        source.h = height;
        source.d = 1;
        const SDL_GPUTextureTransferInfo destination{download_, 0, width, height};
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (!fence)
        {
            fail("Cannot submit the frame");
        }
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        const auto file = screenshot_;
        screenshot_.clear();
        SDL_PixelFormat format = SDL_PIXELFORMAT_UNKNOWN;
        if (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
        {
            format = SDL_PIXELFORMAT_ARGB8888;
        }
        else if (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
        {
            format = SDL_PIXELFORMAT_ABGR8888;
        }
        else
        {
            throw std::runtime_error(
                "Screenshots of this display's pixel format are not supported");
        }
        auto* pixels = SDL_MapGPUTransferBuffer(device_, download_, false);
        if (!pixels)
        {
            fail("Cannot read the screenshot");
        }
        SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width),
            static_cast<int>(height),
            format,
            pixels,
            static_cast<int>(width * 4));
        const bool saved = surface && SDL_SavePNG(surface, utf8(file).c_str());
        SDL_DestroySurface(surface);
        SDL_UnmapGPUTransferBuffer(device_, download_);
        if (!saved)
        {
            fail("Cannot save the screenshot");
        }
    }

    VideoSettings settings_;
    VideoInfo info_;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline* hd_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* people_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* grade_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* glow_pipeline_ = nullptr;
    bool fx_enabled_ = false;
    bool edges_enabled_ = false;
    std::string hd_folder_;
    std::map<std::string, HdArt> hd_art_;
    std::deque<std::string> hd_animation_order_;
    SDL_GPUGraphicsPipeline* scaler_ = nullptr;
    SDL_GPUSampler* scaler_sampler_ = nullptr;
    float scaler_sharpness_ = 0.35f;
    SDL_GPUTexture* frame_texture_ = nullptr;
    SDL_GPUTexture* composite_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    SDL_GPUTransferBuffer* download_ = nullptr;
    Uint32 frame_width_ = 0, frame_height_ = 0, composite_width_ = 0, composite_height_ = 0;
    bool claimed_ = false, overlay_ready_ = false;
    std::filesystem::path screenshot_;
};
} // namespace

std::unique_ptr<Video> create_video(const VideoSettings& settings)
{
    if (settings.headless)
    {
        return create_headless_video(settings);
    }
    return std::make_unique<SdlGpuVideo>(settings);
}

const std::vector<HdGlyph>& hd_glyphs()
{
    return hd_glyphs_list;
}

void set_hd_text(bool enabled)
{
    hd_text_enabled = enabled;
}

} // namespace beachlife::platform

extern "C" void beachlife_hd_begin(void)
{
    beachlife::platform::hd_items.clear();
    beachlife::platform::hd_excluded.clear();
    beachlife::platform::hd_glyphs_list.clear();
    beachlife::platform::hd_people.clear();
    beachlife::platform::hd_world.active = false;
    beachlife::platform::hd_waterfall = false;
}

extern "C" void beachlife_hd_add(
    const char* key, int x, int y, int, int, float scale, int kind)
{
    using namespace beachlife::platform;
    hd_items.push_back({key, x, y, scale, kind});
}

extern "C" void beachlife_hd_world(int origin_x, int origin_y, float scale, float hour, int lighting)
{
    beachlife::platform::hd_world = {true,
        static_cast<float>(origin_x),
        static_cast<float>(origin_y),
        scale,
        hour,
        lighting != 0};
}

extern "C" void beachlife_hd_waterfall(int present)
{
    beachlife::platform::hd_waterfall = present != 0;
}

extern "C" void beachlife_hd_person(int x, int y, float scale)
{
    if (beachlife::platform::hd_people.size() < 400)
    {
        beachlife::platform::hd_people.push_back({x, y, scale});
    }
}

extern "C" void beachlife_hd_cover(int left, int top, int right, int bottom)
{
    using namespace beachlife::platform;
    std::erase_if(hd_glyphs_list, [&](const HdGlyph& glyph) {
        return glyph.x >= left && glyph.x < right && glyph.y >= top && glyph.y < bottom;
    });
}

extern "C" void beachlife_hd_exclude(int left, int top, int right, int bottom)
{
    beachlife::platform::hd_excluded.push_back({static_cast<float>(left),
        static_cast<float>(top),
        static_cast<float>(right),
        static_cast<float>(bottom)});
}

extern "C" int beachlife_hd_text_active(void)
{
    return beachlife::platform::hd_text_enabled && !beachlife::platform::hd_text_paused;
}

extern "C" void beachlife_hd_text_pause(int paused)
{
    beachlife::platform::hd_text_paused = paused;
}

extern "C" void beachlife_hd_glyph(unsigned int codepoint, int x, int y, int width, int height,
    unsigned int colour, int clip_left, int clip_top, int clip_right, int clip_bottom)
{
    using namespace beachlife::platform;
    const auto red = static_cast<unsigned char>(colour >> 16);
    const auto green = static_cast<unsigned char>(colour >> 8);
    const auto blue = static_cast<unsigned char>(colour);
    hd_glyphs_list.push_back(
        {codepoint, x, y, width, height, red, green, blue, clip_left, clip_top, clip_right, clip_bottom});
}
