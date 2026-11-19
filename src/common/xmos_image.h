#pragma once
// XMOS tools<=12 flash boot image: parsing, CRC verification and CRC recomputation.
// Shared by fwpatch and vci400-dfu. Format notes are in fwpatch.cpp and
// docs/technical.md.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace xmos_image {

constexpr uint32_t kTag = 0x01A551E5;
constexpr uint32_t kPoly = 0xEDB88320;
constexpr int kPageWords = 64;
constexpr int kCrcStartWord = 4;

inline uint8_t Reverse(uint8_t v) {
    uint8_t r = 0;
    for (int k = 0; k < 8; ++k)
        if (v & (1u << k)) r |= static_cast<uint8_t>(1u << (7 - k));
    return r;
}

inline std::vector<uint8_t> ReverseAll(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) out[i] = Reverse(in[i]);
    return out;
}

// XS1 crc32 instruction: data bits shift into bit 31, polynomial reduction on carry.
inline uint32_t Step(uint32_t d, uint32_t s) {
    for (int k = 0; k < 32; ++k) {
        uint32_t carry = d & 1;
        d = (d >> 1) | ((s & 1) << 31);
        s >>= 1;
        if (carry) d ^= kPoly;
    }
    return d;
}

inline uint32_t Word(const std::vector<uint8_t>& t, size_t index) {
    uint32_t w;
    memcpy(&w, &t[index * 4], 4);
    return w;
}

inline void SetWord(std::vector<uint8_t>& t, size_t index, uint32_t w) { memcpy(&t[index * 4], &w, 4); }

// Solves Step(state, x) == target. Step is affine: Step(c,x) = Step(c,0) ^ Step(0,x).
inline bool SolveWord(uint32_t state, uint32_t target, uint32_t& x) {
    uint32_t rhs = target ^ Step(state, 0);
    uint32_t rows[32];
    uint32_t rhsBits = rhs;
    for (int r = 0; r < 32; ++r) {
        rows[r] = 0;
        for (int i = 0; i < 32; ++i)
            if (Step(0, 1u << i) & (1u << r)) rows[r] |= 1u << i;
    }
    int pivotOfCol[32];
    int pivotRow = 0;
    for (int c = 0; c < 32; ++c) {
        pivotOfCol[c] = -1;
        int sel = -1;
        for (int r = pivotRow; r < 32; ++r)
            if (rows[r] & (1u << c)) { sel = r; break; }
        if (sel < 0) continue;
        std::swap(rows[sel], rows[pivotRow]);
        uint32_t bs = (rhsBits >> sel) & 1, bp = (rhsBits >> pivotRow) & 1;
        rhsBits = (rhsBits & ~((1u << sel) | (1u << pivotRow))) | (bs << pivotRow) | (bp << sel);
        for (int r = 0; r < 32; ++r)
            if (r != pivotRow && (rows[r] & (1u << c))) {
                rows[r] ^= rows[pivotRow];
                if ((rhsBits >> pivotRow) & 1) rhsBits ^= 1u << r;
            }
        pivotOfCol[c] = pivotRow++;
    }
    if (pivotRow != 32) return false;
    x = 0;
    for (int c = 0; c < 32; ++c)
        if ((rhsBits >> pivotOfCol[c]) & 1) x |= 1u << c;
    return Step(state, x) == target;
}

struct Tile {
    uint32_t offset, flags, words;
};

struct Image {
    std::vector<uint8_t> raw, t;  // t = bit-reversed domain
    uint32_t size = 0;
    std::vector<Tile> tiles;
    std::string error;
};

inline bool FromBytes(std::vector<uint8_t> bytes, Image& img) {
    if (bytes.size() < 256 || bytes.size() > 4 * 1024 * 1024) { img.error = "unexpected image length"; return false; }
    img.raw = std::move(bytes);
    img.t = ReverseAll(img.raw);
    if (Word(img.t, 0) != kTag) { img.error = "not an XMOS tools<=12 image (tag mismatch)"; return false; }
    img.size = Word(img.t, 5);
    uint32_t count = Word(img.t, 6);
    if (img.size % 4 || img.size > img.raw.size() || img.size < kPageWords * 4) { img.error = "bad image size field"; return false; }
    if (count == 0 || count > 8) { img.error = "bad tile count"; return false; }
    uint32_t expect = 4 * (7 + 3 * count);
    img.tiles.clear();
    for (uint32_t i = 0; i < count; ++i) {
        Tile tile{Word(img.t, 7 + 3 * i), Word(img.t, 8 + 3 * i), Word(img.t, 9 + 3 * i)};
        if (tile.offset != expect || tile.words < 2 || tile.offset + 4ull * tile.words > img.size) { img.error = "tile table inconsistent"; return false; }
        expect = tile.offset + 4 * tile.words;
        img.tiles.push_back(tile);
    }
    if (expect != img.size) { img.error = "tiles do not end at image size"; return false; }
    for (size_t i = img.size; i < img.raw.size(); ++i)
        if (img.raw[i] != 0) { img.error = "non-zero padding after image"; return false; }
    return true;
}

inline uint32_t TileState(const Image& img, const Tile& tile) {  // state before the CRC word
    uint32_t c = Step(0xFFFFFFFF, tile.words - 1);
    for (uint32_t i = 0; i + 1 < tile.words; ++i) c = Step(c, Word(img.t, tile.offset / 4 + i));
    return c;
}

inline uint32_t RangeState(const Image& img, uint32_t firstWord, uint32_t endWord) {
    uint32_t c = 0xFFFFFFFF;
    for (uint32_t i = firstWord; i < endWord; ++i) c = Step(c, Word(img.t, i));
    return c;
}

struct Checks {
    std::vector<bool> tileOk;
    bool imageOk = false, pageOk = false;
    bool All() const {
        for (bool b : tileOk)
            if (!b) return false;
        return imageOk && pageOk;
    }
};

inline Checks Verify(const Image& img) {
    Checks c;
    for (const auto& tile : img.tiles)
        c.tileOk.push_back(Step(TileState(img, tile), Word(img.t, tile.offset / 4 + tile.words - 1)) == 0xFFFFFFFF);
    c.imageOk = ~Step(RangeState(img, kCrcStartWord, img.size / 4), Word(img.t, 2)) == 0;
    c.pageOk = ~Step(RangeState(img, kCrcStartWord, kPageWords), Word(img.t, 1)) == 0;
    return c;
}

// Recomputes tile, image and page CRC words in place (tiles first: the image CRC covers them).
inline bool Recompute(Image& img, std::vector<std::string>* log) {
    char line[96];
    for (size_t i = 0; i < img.tiles.size(); ++i) {
        const auto& tile = img.tiles[i];
        uint32_t x;
        if (!SolveWord(TileState(img, tile), 0xFFFFFFFF, x)) return false;
        size_t index = tile.offset / 4 + tile.words - 1;
        if (log && Word(img.t, index) != x) {
            snprintf(line, sizeof line, "tile %zu crc %08X -> %08X", i, Word(img.t, index), x);
            log->push_back(line);
        }
        SetWord(img.t, index, x);
    }
    const struct { size_t word; uint32_t end; const char* name; } headers[] = {
        {2, img.size / 4, "image"}, {1, static_cast<uint32_t>(kPageWords), "page"}};
    for (const auto& h : headers) {
        uint32_t x;
        if (!SolveWord(RangeState(img, kCrcStartWord, h.end), 0xFFFFFFFF, x)) return false;
        if (log && Word(img.t, h.word) != x) {
            snprintf(line, sizeof line, "%s crc %08X -> %08X", h.name, Word(img.t, h.word), x);
            log->push_back(line);
        }
        SetWord(img.t, h.word, x);
    }
    return true;
}

}  // namespace xmos_image
