// Embedded character animation (placeholder).
//
// This repository ships without a character GIF — the framework supports
// any animated GIF but the chosen art is yours to provide.
//
// To plug in your own:
//   1. Place a GIF at assets/your-char.gif (transparent background works
//      best; the encoder composites it onto black).
//   2. Run: python tools/encode_gif.py assets/your-char.gif > src/character_gif.h
//   3. Rebuild and flash.
//
// While CHAR_GIF_LEN is 0 the firmware skips GIF playback gracefully and
// the prompt UI shows just the text + selector pills.
#pragma once
#include <stdint.h>

static constexpr uint16_t CHAR_W = 1;
static constexpr uint16_t CHAR_H = 1;
static constexpr size_t   CHAR_GIF_LEN = 0;
static const uint8_t char_gif[1] = {0};
