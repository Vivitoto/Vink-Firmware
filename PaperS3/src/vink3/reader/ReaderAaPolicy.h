#pragma once

#include "../VinkPaperS3.h"
#include <stdint.h>

namespace vink3 {

enum class ReaderAntialiasProfile : uint8_t {
    Current = 0,
    EdcSoft = 1,
    EdcBalanced = 2,
    EdcCrisp = 3,
};

class ReaderAaPolicy {
public:
    // Vink nibble semantics: 0 = white/skip, 15 = black ink.
    // EDCBook source evidence uses the opposite stored level direction, so all
    // EDC-like profile tables here are adapted for Vink's framebuffer pipeline.
    static uint8_t quantizeCoverage(uint8_t coverage, bool antialias, ReaderAntialiasProfile profile) {
        if (!antialias) return coverage >= 128 ? 15 : 0;
        if (profile == ReaderAntialiasProfile::Current) return (coverage + 8) >> 4;

        uint8_t whiteThreshold = 32;
        uint8_t blackThreshold = 223;
        switch (profile) {
            case ReaderAntialiasProfile::EdcSoft:
                whiteThreshold = 24;
                blackThreshold = 232;
                break;
            case ReaderAntialiasProfile::EdcBalanced:
                whiteThreshold = 32;
                blackThreshold = 223;
                break;
            case ReaderAntialiasProfile::EdcCrisp:
                whiteThreshold = 48;
                blackThreshold = 208;
                break;
            case ReaderAntialiasProfile::Current:
            default:
                break;
        }
        if (coverage <= whiteThreshold) return 0;
        if (coverage >= blackThreshold) return 15;
        const uint16_t num = static_cast<uint16_t>(coverage - whiteThreshold) * 14u;
        const uint16_t den = static_cast<uint16_t>(blackThreshold - whiteThreshold);
        return 1 + static_cast<uint8_t>((num + den / 2) / den);
    }

    static uint8_t mapNibble(uint8_t nibble, ReaderAntialiasProfile profile) {
        static const uint8_t kCurrent[16] __attribute__((aligned(1))) = {
            0, 3, 5, 6, 7, 8, 9, 10, 10, 11, 12, 13, 13, 14, 15, 15
        };
        static const uint8_t kSoft[16] __attribute__((aligned(1))) = {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        };
        static const uint8_t kBalanced[16] __attribute__((aligned(1))) = {
            0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 12, 13, 15, 15
        };
        static const uint8_t kCrisp[16] __attribute__((aligned(1))) = {
            0, 0, 0, 0, 0, 4, 6, 6, 8, 10, 12, 14, 15, 15, 15, 15
        };
        const uint8_t idx = nibble & 0x0F;
        switch (profile) {
            case ReaderAntialiasProfile::EdcSoft: return kSoft[idx];
            case ReaderAntialiasProfile::EdcBalanced: return kBalanced[idx];
            case ReaderAntialiasProfile::EdcCrisp: return kCrisp[idx];
            case ReaderAntialiasProfile::Current:
            default: return kCurrent[idx];
        }
    }

    static uint16_t rgb565ForNibble(uint8_t nibble, uint16_t color, bool antialias, ReaderAntialiasProfile profile) {
        if (color == TFT_WHITE) return TFT_WHITE;
        if (color != TFT_BLACK) return color;
        if (!antialias) return (nibble >= 8) ? TFT_BLACK : TFT_WHITE;
        static const uint16_t k4BitToRgb565[16] __attribute__((aligned(2))) = {
            0xFFFF, 0xDEDB, 0xC618, 0xAD75, 0x9CD3, 0x8C51, 0x7BCF, 0x6B4D,
            0x5ACB, 0x4A69, 0x39E7, 0x3186, 0x2124, 0x10A2, 0x0841, 0x0000
        };
        return k4BitToRgb565[mapNibble(nibble, profile) & 0x0F];
    }

    static ReaderAntialiasProfile fromU8(uint8_t v) {
        switch (v & 0x3) {
            case 1: return ReaderAntialiasProfile::EdcSoft;
            case 2: return ReaderAntialiasProfile::EdcBalanced;
            case 3: return ReaderAntialiasProfile::EdcCrisp;
            case 0:
            default: return ReaderAntialiasProfile::Current;
        }
    }
};

} // namespace vink3
