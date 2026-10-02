// RP - C++ remake of RWX (Rusted Warfare). Common utilities.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <algorithm>

// World/logic constants. Logic runs on discrete ticks like RWX (60 ticks/sec).
// Distances are in world pixels; one map tile = TILE_SIZE px.
static const int TILE_SIZE = 20;
static const float TICK_RATE = 60.0f;   // logic updates per second

inline std::string TrimRight(const std::string& s) {
    size_t e = s.find_last_not_of(" \t\r\n");
    return (e == std::string::npos) ? std::string() : s.substr(0, e + 1);
}
inline std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
inline std::string ToLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline bool EndsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// Small deterministic RNG (xorshift32), used for jitter/explosions.
struct Rng {
    uint32_t s = 0x9e3779b9u;
    uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
    }
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
};
extern Rng g_rng;
extern std::string g_assetsRoot; // absolute or relative path to assets/ (no trailing slash)

struct Vec2 {
    float x = 0, y = 0;
    Vec2() {}
    template <class A, class B> Vec2(A a, B b) : x((float)a), y((float)b) {}
    Vec2 operator+(const Vec2& o) const { return { x + o.x, y + o.y }; }
    Vec2 operator-(const Vec2& o) const { return { x - o.x, y - o.y }; }
    Vec2 operator*(float k) const { return { x * k, y * k }; }
    float len() const { return std::sqrt(x * x + y * y); }
    float dist(const Vec2& o) const { return (*this - o).len(); }
    Vec2 norm() const { float l = len(); return l > 1e-6f ? Vec2{ x / l, y / l } : Vec2{}; }
    static float angleTo(const Vec2& from, const Vec2& to) { return std::atan2(to.y - from.y, to.x - from.x); }
};

// ---- Base64 decode (for Tiled .tmx layer data) ----
inline std::vector<uint8_t> Base64Decode(const std::string& in) {
    static int8_t T[256]; static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)a[i]] = (int8_t)i;
        init = true;
    }
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

// ---- Tiny inflate (raw DEFLATE) - public-domain style, minimal ----
// Supports stored + fixed + dynamic Huffman blocks; enough for gzip'd TMX layers.
inline std::vector<uint8_t> InflateRaw(const uint8_t* data, size_t n) {
    struct Reader {
        const uint8_t* p; size_t n; size_t bitpos = 0;
        int bit() {
            if (bitpos >= n * 8) return -1;
            int b = (p[bitpos >> 3] >> (bitpos & 7)) & 1; bitpos++; return b;
        }
        int bits(int count) {
            int v = 0;
            for (int i = 0; i < count; ++i) { int b = bit(); if (b < 0) return -1; v |= b << i; }
            return v;
        }
    };
    Reader r{ data, n };
    std::vector<uint8_t> out;
    struct Huff {
        int counts[16]; std::vector<uint16_t> symbols;
        void build(const std::vector<uint8_t>& lengths) {
            for (int i = 0; i < 16; ++i) counts[i] = 0;
            for (uint8_t l : lengths) counts[l]++;
            counts[0] = 0;
            int offs[16]; offs[0] = 0; offs[1] = 0;
            for (int i = 1; i < 15; ++i) offs[i + 1] = offs[i] + counts[i];
            symbols.assign(lengths.size(), 0);
            for (size_t i = 0; i < lengths.size(); ++i)
                if (lengths[i]) symbols[offs[lengths[i]]++] = (uint16_t)i;
        }
    };
    auto decodeSym = [&](const Huff& h) -> int {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            int b = r.bit(); if (b < 0) return -1;
            code |= b;
            int count = h.counts[len];
            if (code - first < count) return h.symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    };
    // Length / distance base tables
    static const int lenBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const int lenExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const int distBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const int distExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

    Huff fixedLit, fixedDist;
    { std::vector<uint8_t> l(288);
      for (int i = 0; i <= 143; ++i) l[i] = 8;
      for (int i = 144; i <= 255; ++i) l[i] = 9;
      for (int i = 256; i <= 279; ++i) l[i] = 7;
      for (int i = 280; i <= 287; ++i) l[i] = 8;
      fixedLit.build(l);
      std::vector<uint8_t> d(30, 5); fixedDist.build(d); }

    while (true) {
        int final_ = r.bit();
        int type = r.bits(2);
        if (type < 0) break;
        if (type == 0) { // stored
            r.bitpos = (r.bitpos + 7) & ~7u;
            if (r.bitpos / 8 + 4 > n) break;
            uint16_t len = r.p[r.bitpos / 8] | (r.p[r.bitpos / 8 + 1] << 8);
            r.bitpos += 32;
            for (int i = 0; i < len; ++i) out.push_back(r.p[r.bitpos / 8 + i]);
            r.bitpos += (size_t)len * 8;
        } else {
            Huff lit, dist;
            if (type == 1) { lit = fixedLit; dist = fixedDist; }
            else if (type == 2) { // dynamic
                int hlit = r.bits(5) + 257, hdist = r.bits(5) + 1, hclen = r.bits(4) + 4;
                static const int ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                uint8_t clen[19] = { 0 };
                for (int i = 0; i < hclen; ++i) clen[ord[i]] = (uint8_t)r.bits(3);
                Huff clhuff; { std::vector<uint8_t> cv(19); for (int i = 0; i < 19; ++i) cv[i] = clen[i]; clhuff.build(cv); }
                std::vector<uint8_t> lengths;
                while ((int)lengths.size() < hlit + hdist) {
                    int sym = decodeSym(clhuff);
                    if (sym < 16) lengths.push_back((uint8_t)sym);
                    else if (sym == 16) { int rep = r.bits(2) + 3; uint8_t prev = lengths.empty() ? 0 : lengths.back(); for (int i = 0; i < rep; ++i) lengths.push_back(prev); }
                    else if (sym == 17) { int rep = r.bits(3) + 3; for (int i = 0; i < rep; ++i) lengths.push_back(0); }
                    else if (sym == 18) { int rep = r.bits(7) + 11; for (int i = 0; i < rep; ++i) lengths.push_back(0); }
                    else break;
                }
                std::vector<uint8_t> lv(hlit), dv(hdist);
                for (int i = 0; i < hlit; ++i) lv[i] = lengths[i];
                for (int i = 0; i < hdist; ++i) dv[i] = lengths[hlit + i];
                lit.build(lv); dist.build(dv);
            } else break;

            while (true) {
                int sym = decodeSym(lit);
                if (sym < 0) break;
                if (sym < 256) { out.push_back((uint8_t)sym); }
                else if (sym == 256) break;
                else {
                    sym -= 257;
                    int length = lenBase[sym] + r.bits(lenExtra[sym]);
                    int dsym = decodeSym(dist);
                    int distance = distBase[dsym] + r.bits(distExtra[dsym]);
                    size_t start = out.size() - distance;
                    for (int i = 0; i < length; ++i) out.push_back(out[start + i]);
                }
            }
        }
        if (final_) break;
    }
    return out;
}

// gunzip: strip 10-byte header, inflate raw, ignore trailer.
inline std::vector<uint8_t> GzipInflate(const std::vector<uint8_t>& gz) {
    if (gz.size() < 10) return {};
    size_t off = 0;
    if (gz.size() > 2 && gz[0] == 0x1f && gz[1] == 0x8b) { off = 10; if (gz.size() > 3) { uint8_t flg = gz[3]; if (flg & 4) { off += 2 + (gz[off] | (gz[off + 1] << 8)); } if (flg & 8) { while (off < gz.size() && gz[off]) off++; off++; } if (flg & 16) { while (off < gz.size() && gz[off]) off++; off++; } if (flg & 2) off += 2; } }
    return InflateRaw(gz.data() + off, gz.size() - off - 8);
}
