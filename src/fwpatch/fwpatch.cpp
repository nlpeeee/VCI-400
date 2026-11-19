// fwpatch: verify and patch Vestax VCI-400 XMOS audio firmware images (offline).
//
// Fixes the MIDI class-specific AudioControl header (baInterfaceNr 1 -> 4) so that
// Microsoft's usbmidi2 finds the MIDIStreaming interface, then recomputes every CRC
// the XMOS flash loader checks.
//
// Image format (XMOS tools <= 12, verified against vci400_d006_AUDIO.bin and
// vci400_v21_AUDIO.bin):
//   * every flash byte is bit-reversed (sortBits); words are little-endian after reversal
//   * header words: [0] tag 0x01A551E5, [1] page CRC, [2] image CRC, [3] features,
//     [4] version, [5] size (bytes), [6] tile count N, then N x {offset, flags, words}
//   * tile CRC: crc=~0, feed (words-1), feed all tile words incl. trailing CRC -> 0xFFFFFFFF
//   * image CRC: crc=~0, feed words 4..size/4-1, feed [2] -> ~crc == 0
//   * page CRC:  crc=~0, feed words 4..63,       feed [1] -> ~crc == 0
//   CRC step = XS1 crc32 instruction, polynomial 0xEDB88320.
// References: xcore/sc_flash module_flash/src/flashlib.c (tags, sortBits, header fields),
// xmos/xcore_iot test/usb/tinyusb_demos/dfu_runtime/src/flash_boot_image.c (page CRC).
//
//   fwpatch verify   <image.bin>
//   fwpatch selftest <image.bin>            recompute shipped CRCs from scratch
//   fwpatch patch    <in.bin> <out.bin> [--manifest out.json]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xmos_image.h"

using namespace xmos_image;

namespace {

// Interface 3 (MIDI AudioControl) descriptor, its class-specific header, and the start of
// interface 4 (MIDIStreaming). Offset 17 is baInterfaceNr[0].
const uint8_t kWindow[] = {0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
                           0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01,
                           0x09, 0x04, 0x04, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00};
constexpr size_t kPatchIndex = 17;
constexpr uint8_t kOldValue = 0x01, kNewValue = 0x04;

bool Load(const char* path, Image& img) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) { img.error = "cannot open file"; return false; }
    std::vector<uint8_t> bytes;
    uint8_t buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, f)) > 0) bytes.insert(bytes.end(), buffer, buffer + got);
    fclose(f);
    return FromBytes(std::move(bytes), img);
}

void PrintChecks(const char* label, const Image& img, const Checks& c) {
    printf("%s: tag %08X size 0x%X tiles %zu\n", label, kTag, img.size, img.tiles.size());
    for (size_t i = 0; i < img.tiles.size(); ++i)
        printf("  tile %zu: offset 0x%X words 0x%X flags %08X crc %08X %s\n", i, img.tiles[i].offset, img.tiles[i].words,
               img.tiles[i].flags, Word(img.t, img.tiles[i].offset / 4 + img.tiles[i].words - 1), c.tileOk[i] ? "PASS" : "FAIL");
    printf("  image crc %08X %s\n  page  crc %08X %s\n", Word(img.t, 2), c.imageOk ? "PASS" : "FAIL", Word(img.t, 1), c.pageOk ? "PASS" : "FAIL");
}

std::string Sha256(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    uint8_t hash[32]{};
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), hash, sizeof hash) == 0) {
            char hex[3];
            for (uint8_t b : hash) { snprintf(hex, sizeof hex, "%02X", b); out += hex; }
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}

std::string JsonEscape(const char* text) {
    std::string out;
    for (const char* p = text; *p; ++p) {
        if (*p == '\\' || *p == '"') out += '\\';
        out += *p;
    }
    return out;
}

int CmdVerify(const char* path) {
    Image img;
    if (!Load(path, img)) { printf("FAIL %s: %s\n", path, img.error.c_str()); return 1; }
    Checks c = Verify(img);
    PrintChecks(path, img, c);
    printf("%s verify %s\n", c.All() ? "PASS" : "FAIL", path);
    return c.All() ? 0 : 1;
}

int CmdSelfTest(const char* path) {
    Image img;
    if (!Load(path, img)) { printf("FAIL %s: %s\n", path, img.error.c_str()); return 1; }
    Checks before = Verify(img);
    PrintChecks(path, img, before);
    if (!before.All()) { printf("FAIL selftest: shipped image does not verify\n"); return 1; }
    Image copy = img;
    // Zero every CRC word, then recompute from scratch: must reproduce the shipped bytes.
    for (const auto& tile : copy.tiles) SetWord(copy.t, tile.offset / 4 + tile.words - 1, 0);
    SetWord(copy.t, 1, 0);
    SetWord(copy.t, 2, 0);
    if (!Recompute(copy, nullptr)) { printf("FAIL selftest: CRC solve failed\n"); return 1; }
    bool same = copy.t == img.t;
    printf("%s selftest %s: recomputed CRCs %s the shipped image\n", same ? "PASS" : "FAIL", path, same ? "reproduce" : "DO NOT reproduce");
    return same ? 0 : 1;
}

int CmdPatch(const char* in, const char* out, const char* manifest) {
    Image img;
    if (!Load(in, img)) { printf("FAIL %s: %s\n", in, img.error.c_str()); return 1; }
    Checks before = Verify(img);
    PrintChecks(in, img, before);
    if (!before.All()) { printf("FAIL patch: input does not verify; refusing\n"); return 1; }

    std::vector<size_t> hits;
    for (size_t i = 0; i + sizeof kWindow <= img.size; ++i)
        if (memcmp(&img.t[i], kWindow, sizeof kWindow) == 0) hits.push_back(i);
    if (hits.size() != 1) { printf("FAIL patch: descriptor window found %zu times (need exactly 1)\n", hits.size()); return 1; }
    size_t site = hits[0] + kPatchIndex;
    int tileIndex = -1;
    for (size_t i = 0; i < img.tiles.size(); ++i) {
        const auto& tile = img.tiles[i];
        if (site >= tile.offset && site < tile.offset + 4ull * (tile.words - 1)) tileIndex = static_cast<int>(i);
    }
    if (tileIndex < 0) { printf("FAIL patch: site 0x%zX is not inside tile data\n", site); return 1; }
    if (img.t[site] != kOldValue) { printf("FAIL patch: byte at 0x%zX is %02X, expected %02X\n", site, img.t[site], kOldValue); return 1; }

    Image patched = img;
    patched.t[site] = kNewValue;
    std::vector<std::string> log;
    if (!Recompute(patched, &log)) { printf("FAIL patch: CRC solve failed\n"); return 1; }
    Checks after = Verify(patched);
    patched.raw = ReverseAll(patched.t);
    PrintChecks(out, patched, after);
    if (!after.All()) { printf("FAIL patch: patched image does not verify\n"); return 1; }

    // Only the descriptor byte and recomputed CRC words may differ.
    std::vector<size_t> diff;
    for (size_t i = 0; i < img.raw.size(); ++i)
        if (img.raw[i] != patched.raw[i]) diff.push_back(i);
    auto allowed = [&](size_t i) {
        if (i == site || i / 4 == 1 || i / 4 == 2) return true;
        for (const auto& tile : img.tiles)
            if (i / 4 == tile.offset / 4 + tile.words - 1) return true;
        return false;
    };
    for (size_t i : diff)
        if (!allowed(i)) { printf("FAIL patch: unexpected change at 0x%zX\n", i); return 1; }

    FILE* f = nullptr;
    if (fopen_s(&f, out, "wb") || !f) { printf("FAIL patch: cannot write %s\n", out); return 1; }
    fwrite(patched.raw.data(), 1, patched.raw.size(), f);
    fclose(f);

    std::string inHash = Sha256(img.raw), outHash = Sha256(patched.raw);
    printf("descriptor byte: reversed offset 0x%zX (tile %d), raw byte %02X -> %02X\n", site, tileIndex, img.raw[site], patched.raw[site]);
    for (const auto& line : log) printf("%s\n", line.c_str());
    printf("changed bytes: %zu\ninput  sha256 %s\noutput sha256 %s\n", diff.size(), inHash.c_str(), outHash.c_str());

    if (manifest) {
        FILE* m = nullptr;
        if (fopen_s(&m, manifest, "wb") == 0 && m) {
            fprintf(m, "{\n  \"tool\": \"fwpatch\",\n  \"input\": \"%s\",\n  \"input_sha256\": \"%s\",\n", JsonEscape(in).c_str(), inHash.c_str());
            fprintf(m, "  \"output\": \"%s\",\n  \"output_sha256\": \"%s\",\n  \"image_size\": %u,\n", JsonEscape(out).c_str(), outHash.c_str(), img.size);
            fprintf(m, "  \"descriptor_site_reversed_offset\": %zu,\n  \"tile\": %d,\n  \"baInterfaceNr\": [%u, %u],\n", site, tileIndex, kOldValue, kNewValue);
            fprintf(m, "  \"raw_byte\": [%u, %u],\n  \"changed_byte_offsets\": [", img.raw[site], patched.raw[site]);
            for (size_t i = 0; i < diff.size(); ++i) fprintf(m, "%s%zu", i ? ", " : "", diff[i]);
            fprintf(m, "],\n  \"crc_changes\": [");
            for (size_t i = 0; i < log.size(); ++i) fprintf(m, "%s\"%s\"", i ? ", " : "", log[i].c_str());
            fprintf(m, "],\n  \"verified\": true\n}\n");
            fclose(m);
        }
    }
    printf("PASS patch %s -> %s\n", in, out);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "verify")) return CmdVerify(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "selftest")) return CmdSelfTest(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "patch")) {
        const char* manifest = (argc >= 6 && !strcmp(argv[4], "--manifest")) ? argv[5] : nullptr;
        return CmdPatch(argv[2], argv[3], manifest);
    }
    printf("usage: fwpatch verify <image> | selftest <image> | patch <in> <out> [--manifest m.json]\n");
    return 2;
}
