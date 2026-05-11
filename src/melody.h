// Alert chime played through M5.Speaker on permission-prompt arrival.
//
// Default: a short ascending C major arpeggio (C5-E5-G5). Replace with
// your own melody — the format is a flat list of {frequency_Hz,
// duration_ms} pairs. Use N_REST (frequency = 0) for rests.
#pragma once
#include <stdint.h>

struct Note { float freq; uint32_t ms; };

static constexpr float N_REST = 0.0f;
static constexpr float N_C5   = 523.25f;
static constexpr float N_E5   = 659.25f;
static constexpr float N_G5   = 783.99f;

static const Note PROMPT_MELODY[] = {
    {N_C5, 180},
    {N_E5, 180},
    {N_G5, 360},
};
static constexpr size_t PROMPT_MELODY_LEN =
    sizeof(PROMPT_MELODY) / sizeof(PROMPT_MELODY[0]);
