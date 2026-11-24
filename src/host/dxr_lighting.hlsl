// DXR 1.1 inline ray tracing for the presentation-only room lighting.
// Cabinet projection: world (x, y, z) appears at screen (x, y - z).
// Output (HDR): [0,1024) ground/wall irradiance, a = ambient occlusion;
// [1024,2048) sprite irradiance, a = sprite tag; rows 256+ hold specular.
#define MAX_LIGHTS 64
struct Light {
    float4 pos;   // xyz, radius
    float4 color; // rgb, intensity
    float4 dir;   // spot axis, cos outer (<= -1 omni)
    float4 shape; // cos inner, source size, pattern, shadow samples
};
cbuffer Scene : register(b0) {
    float4 extent;    // width, height, wing offset, arcade frame
    float4 floorRect; // current room floor in light-map space
    float4 ambient;   // rgb, floor gloss
    float4 world;     // camera x, camera y, light count, flags (1 coverage, 2 specular rows)
    float4 tone;      // actor floor, occlusion strength, first boss + 1, actor count
    float4 previousFloor, previousAmbient, previousTone; // old tone.z is light count
    Light lights[MAX_LIGHTS];
};
RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer coverage : register(t1);
StructuredBuffer<float4> actors : register(t2);
RWTexture2D<float4> lightMap : register(u0);

static const uint NoSelf = 0xffffffffu;
static const uint SpriteSurface = 0xfffffffeu;
static const float3 ViewDir = float3(0, .70710678, .70710678);
static const float Tau = 6.2831853;

uint hashU(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
// World-locked hashes keep penumbra and contact dither fixed to the floor during pans.
float hash2(float2 p) {
    int2 q = int2(floor(p));
    return float(hashU(uint(q.x) * 1597334677u ^ uint(q.y) * 3812015801u) & 0xffffff) / 16777216.;
}
float vnoise(float2 p) {
    float2 i = floor(p), f = frac(p), s = f * f * (3 - 2 * f);
    float a = hash2(i), b = hash2(i + float2(1, 0)), c = hash2(i + float2(0, 1)), d = hash2(i + 1);
    return lerp(lerp(a, b, s.x), lerp(c, d, s.x), s.y);
}
bool spriteAt(int2 p) {
    if (any(p < 0) || p.x >= int(extent.x) || p.y >= int(extent.y)) return false;
    uint bits = coverage.Load((uint(p.y) * 32 + (uint(p.x) >> 5)) * 4);
    return ((bits >> (uint(p.x) & 31)) & 1) != 0;
}
uint spriteOwner(int2 pixel){
    uint address=(uint(extent.y)*32+uint(pixel.y)*256+(uint(pixel.x)>>2))*4;
    return (coverage.Load(address)>>((uint(pixel.x)&3)*8))&255;
}
// Card-local u (across) and v (up) map back to the displayed sprite pixel.
bool silhouette(uint id, float3 local) {
    float4 r = actors[id * 3];
    float u = local.x, v = local.z;
    if ((uint(world.w) & 1) == 0) {
        float2 q = float2((u - .5) / .42, (v - .48) / .52);
        return dot(q, q) < 1;
    }
    int2 pixel=int2(floor(r.x + u * r.y), floor(r.z - v * r.w - .001));
    if(!spriteAt(pixel))return false;
    if((uint(world.w)&4)!=0){
        uint bank=spriteOwner(pixel);
        if(bank!=uint(actors[id*3+1].x))return false;
    }
    return true;
}
float visibility(float3 origin, float3 dir, float tmax, uint self, uint occluders) {
    // An ungrouped 2D sprite has no evidenced world-space height. Its shared
    // light field can cross a wall proxy (notably y=46 minus height=24), making
    // a straight shadow cut through Cobra's head. Do not receive proxy shadows
    // on that artistic surface. Its alpha-tested cards still shadow the room.
    if(self==SpriteSurface)return 1;
    RayDesc ray;
    ray.Origin = origin; ray.Direction = dir; ray.TMin = .02; ray.TMax = max(.03, tmax);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, occluders, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            uint id = q.CandidateInstanceID();
            if (id == self) continue;
            // Adjacent tiles of one boss are not separate physical occluders.
            if (self < SpriteSurface && actors[self*3+1].w>0 && actors[id*3+1].w==actors[self*3+1].w) continue;
            float3 local = q.CandidateObjectRayOrigin() + q.CandidateObjectRayDirection() * q.CandidateTriangleRayT();
            if (silhouette(id, local)) q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_NOTHING ? 1. : 0.;
}
float gobo(uint pattern, float3 P, Light L, float t) {
    float2 w = P.xy + world.xy;
    if (pattern == 1) { // canopy: leaves drift across the sun
        float2 c = w * .075 + float2(t * .0021, t * .0013);
        float n = vnoise(c) * .6 + vnoise(c * 2.3 + 7.1) * .4;
        return lerp(.42, 1.15, smoothstep(.34, .64, n));
    }
    if (pattern == 2) { // ceiling fan blades turning under the lamp
        float2 d = P.xy - L.pos.xy;
        float a = atan2(d.y, d.x) + t * .055;
        float blade = abs(frac(a * 3 / Tau) - .5) * 2;
        return lerp(.32, 1.08, smoothstep(.22, .42, blade) * saturate(length(d) * .08));
    }
    if (pattern == 3) { // mirror ball: rotating sparkle field
        float2 d = P.xy - L.pos.xy;
        float a = atan2(d.y, d.x) + t * .012, r = length(d);
        float2 cell = float2(a * 11, r * .07), idc = floor(cell), f = frac(cell) - .5;
        float on = step(.52, hash2(idc + 31));
        float s = saturate(1 - length(f) * 3.1);
        return .3 + 2.2 * on * s * s;
    }
    if (pattern == 4) { // caustic shimmer from a leaking tank
        float2 c = w * .055 + t * .009;
        float n = abs(vnoise(c) - vnoise(c * 1.7 + 3.3 + t * .007));
        return .55 + 1.15 * saturate(1 - n * 4.5);
    }
    return 1;
}
float3 diskOffset(uint k, float rot, float3 t1, float3 t2) {
    float r = sqrt((float(k) + .5) / 8.), th = float(k) * 2.39996323 + rot;
    return (t1 * cos(th) + t2 * sin(th)) * r;
}
float shadowRay(float3 P, float3 N, float3 target, uint self, uint occluders) {
    float3 d = target - P;
    float len = max(length(d), 1e-3);
    return visibility(P + N * .35, d / len, len - .5, self, occluders);
}
void shadeLight(uint i, float3 P, float3 N, uint self, float gloss, float shine, float2 seed,
                float time, uint occluders, inout float3 diffuse, inout float3 spec) {
    Light L = lights[i];
    float3 delta = L.pos.xyz - P;
    float dist = max(length(delta), .25);
    float3 dir = delta / dist;
    float x = dist / L.pos.w;
    if (x >= 1) return;
    float x2 = x * x, win = saturate(1 - x2 * x2);
    float att = L.color.w * win * win / (1 + x2 * 5);
    if (L.dir.w > -1) {
        att *= smoothstep(L.dir.w, L.shape.x, dot(-dir, L.dir.xyz));
        if (att <= 0) return;
    }
    float wrap = saturate((dot(N, dir) + .12) / 1.12);
    float s = gloss * pow(saturate(dot(N, normalize(dir + ViewDir))), shine);
    if (att * (wrap + s) < .0025) return;
    uint pattern = uint(L.shape.z);
    if (pattern) att *= gobo(pattern, P, L, time);
    float vis = 1;
    uint samples = min(uint(L.shape.w), 3u);
    if (samples) {
        float3 t1 = normalize(cross(dir, abs(dir.z) < .95 ? float3(0, 0, 1) : float3(1, 0, 0)));
        float3 t2 = cross(dir, t1);
        float rot = hash2(seed) * Tau, radius = samples > 1 ? L.shape.y * .82 : 0;
        if (radius <= 0) {
            vis = shadowRay(P, N, L.pos.xyz, self, occluders);
        } else {
            // Area light: golden-angle (Vogel) disk with a per-pixel rotation.
            // Probe the rim first; fully lit or fully shadowed pixels stop
            // there, and only penumbra pixels refine to eight samples
            // (nine smooth levels instead of the old two or three).
            static const uint order[8] = {7, 3, 5, 1, 6, 2, 4, 0};
            float sum = 0;
            uint k = 0;
            for (; k < samples; ++k)
                sum += shadowRay(P, N, L.pos.xyz + diskOffset(order[k], rot, t1, t2) * radius, self, occluders);
            if (sum > 0 && sum < float(samples))
                for (; k < 8; ++k)
                    sum += shadowRay(P, N, L.pos.xyz + diskOffset(order[k], rot, t1, t2) * radius, self, occluders);
            vis = sum / float(k);
        }
    }
    diffuse += L.color.rgb * att * wrap * vis;
    spec += L.color.rgb * att * s * vis;
}
float occlusion(float3 P, float3 N, uint self, float2 seed, float strength, uint occluders) {
    if (strength <= 0) return 1;
    float3 t1 = abs(N.z) > .5 ? float3(1, 0, 0) : float3(0, 0, 1);
    float3 t2 = cross(N, t1);
    float a = hash2(seed + 17.3) * Tau, hit = 0;
    [unroll] for (uint k = 0; k < 3; ++k) {
        float ang = a + Tau * k / 3;
        float3 d = normalize(N + (t1 * cos(ang) + t2 * sin(ang)) * .9);
        hit += 1 - visibility(P + N * .3, d, 28, self, occluders);
    }
    return 1 - strength * hit / 3 * .62;
}
// Side scenery is painted into the background. The narrow side boxes cast
// shadows into the arena, but do not describe a raised strip in that paint.
void surfaceAt(float2 px, float4 f, out float3 P, out float3 N) {
    N = float3(0, 0, 1);
    P = float3(px, 0);
    bool spanX = px.x >= f.x - 30 && px.x <= f.x + f.z + 30;
    if (spanX && px.y < f.y && px.y >= f.y - 44) {
        P = float3(px.x, f.y + .6, f.y - px.y);
        N = float3(0, 1, 0);
        return;
    }
    // Front wall: the art's wall face, its shading strip and the border below start at
    // the floor edge. The lip proxy box is solid there, so a ground receiver would sit
    // inside it and every shadow and occlusion ray would hit it at once (black bar).
    // Light it as a wall top. Rows above the edge are floor art.
    if (spanX && px.y >= f.y + f.w)
        P = float3(px.x, px.y + 14, 14.4);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(extent.xy))) return;
    float2 pixel = id.xy + .5;
    float2 seed = floor(pixel) + world.xy;
    uint total = min(uint(world.z), MAX_LIGHTS);
    uint currentCount = total - uint(previousTone.z);
    bool departing = previousFloor.z > 0 &&
        all(pixel >= previousFloor.xy - float2(30, 44)) &&
        all(pixel < previousFloor.xy + previousFloor.zw + float2(30, 15));
    uint first = departing ? currentCount : 0;
    uint limit = departing ? total : currentCount;
    float4 roomAmbient = departing ? previousAmbient : ambient;
    float2 roomTone = departing ? previousTone.xy : tone.xy;
    float lightTime = departing ? previousTone.w : extent.w;
    uint occluders = departing ? 5 : 3; // actor cards plus the room's walls
    bool specRows = (uint(world.w) & 2) != 0;

    float3 P, N;
    float4 roomFloor = departing ? previousFloor : floorRect;
    surfaceAt(pixel, roomFloor, P, N);
    // Keep actor shadows on side artwork. Tracing the solid wall proxies
    // from this painted surface would start some rays inside their own wall.
    uint backgroundOccluders = occluders;
    if (pixel.y >= roomFloor.y && pixel.y < roomFloor.y + roomFloor.w &&
        (pixel.x < roomFloor.x || pixel.x >= roomFloor.x + roomFloor.z))
        backgroundOccluders = 1;
    // The front rail and the raised door cap are in front of the actors.
    if (roomFloor.z > 0 && pixel.x >= roomFloor.x - 30 && pixel.x < roomFloor.x + roomFloor.z + 30 &&
        pixel.y >= roomFloor.y + roomFloor.w && pixel.y < roomFloor.y + roomFloor.w + 15)
        backgroundOccluders &= ~1u;
    float ao = occlusion(P, N, NoSelf, seed, roomTone.y, backgroundOccluders);
    float3 diffuse = 0, spec = 0;
    for (uint i = first; i < limit; ++i)
        shadeLight(i, P, N, NoSelf, roomAmbient.w, 22, seed, lightTime, backgroundOccluders, diffuse, spec);
    lightMap[id.xy] = float4(roomAmbient.rgb * ao + diffuse, ao);
    if (specRows) lightMap[id.xy + uint2(0, 256)] = float4(spec, 0);

    float4 actorLight = 0;
    float3 actorSpec = 0;
    if ((uint(world.w) & 1) == 0 || spriteAt(int2(id.xy))) {
        uint a=NoSelf;
        float3 Pa=0;
        // The displayed boss silhouette is authoritative. Do not expose the
        // current object rectangles where displayed parts overlap or leave a
        // narrow gap during animation. The palette still protects other actors.
        if((uint(world.w)&4)!=0 && tone.z>0){
            uint owner=spriteOwner(int2(id.xy));
            for(uint candidate=uint(tone.z)-1;candidate<uint(tone.w);++candidate){
                float4 info=actors[candidate*3+1],r=actors[candidate*3+2];
                if(info.w>0 && uint(info.x)==owner &&
                   pixel.x>=r.x-8 && pixel.x<r.x+r.y+8 && pixel.y>=r.z-r.w-8 && pixel.y<r.z+8){
                    a=candidate;break;
                }
            }
        }
        // Every displayed sprite gets a continuous receiver, even when the
        // observer's part rectangles have gaps. Do not merge Cobra's separate
        // snakes or unrelated actors merely because they share a palette.
        bool continuousSprite=(uint(world.w)&4)!=0 && spriteOwner(int2(id.xy))>0;
        if(a==NoSelf && !continuousSprite){
        RayDesc eye;
        eye.Origin = float3(pixel.x, pixel.y + 200, 200);
        eye.Direction = float3(0, -.70710678, -.70710678);
        eye.TMin = 0; eye.TMax = 420;
        RayQuery<RAY_FLAG_NONE> primary;
        primary.TraceRayInline(scene, RAY_FLAG_NONE, 1, eye);
        while (primary.Proceed()) {
            if (primary.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
                float3 local = primary.CandidateObjectRayOrigin() +
                               primary.CandidateObjectRayDirection() * primary.CandidateTriangleRayT();
                if (silhouette(primary.CandidateInstanceID(), local)) primary.CommitNonOpaqueTriangleHit();
            }
        }
        if (primary.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            a = primary.CommittedInstanceID();
            Pa = eye.Origin + eye.Direction * primary.CommittedRayT();
        }
        }
        if(a!=NoSelf || continuousSprite){
            float4 r = 0, info = float4(continuousSprite ? spriteOwner(int2(id.xy)) : 0,0,0,0);
            if(a!=NoSelf){r=actors[a*3+2];info=actors[a*3+1];}
            bool assembledBoss=info.w>0;
            if(assembledBoss)Pa=float3(pixel.x,r.z,r.z-pixel.y);
            // Rounded body: a cylinder about the sprite's vertical axis, tipped up near the head.
            float u = saturate((pixel.x - r.x) / max(r.y, 1.)), v = saturate((r.z - pixel.y) / max(r.w, 1.));
            float th = (u - .5) * 2.5;
            float3 Na = normalize(float3(sin(th), cos(th) * .8, .3 + .5 * v));
            if(continuousSprite && !assembledBoss){
                Pa=float3(pixel.x,pixel.y+24,24);
                Na=normalize(float3(0,1,1));
                a=SpriteSurface;
                info.y=0;
            }
            uint material = uint(round(info.y));
            bool polished = !assembledBoss && (material == 2 || material == 3);
            bool bossFlesh = assembledBoss || material == 5;
            float3 diff = 0;
            float gloss = polished ? (material == 2 ? .98 : .78) :
                          bossFlesh ? .58 : material == 4 ? .18 : .38;
            float shine = polished ? (material == 2 ? 24 : 18) :
                          bossFlesh ? 11 : material == 4 ? 5 : 8;
            for (uint i = first; i < limit; ++i)
                shadeLight(i, Pa + Na * .2, Na, a, gloss, shine, seed, lightTime, occluders, diff, actorSpec);
            float3 base = max(roomAmbient.rgb, roomTone.x);
            actorLight = float4(base + diff, (info.x + (polished ? 128 : 0)) / 255.);
        }
    }
    lightMap[id.xy + uint2(1024, 0)] = actorLight;
    if (specRows) lightMap[id.xy + uint2(1024, 256)] = float4(actorSpec, 0);
}
