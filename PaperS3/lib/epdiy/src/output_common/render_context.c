#include "render_context.h"

#include <string.h>
#include "esp_log.h"

#include "../epdiy.h"
#include "lut.h"
#include "render_method.h"

/// For waveforms without timing and the I2S diving method,
/// the default hold time for each line is 12us
const static int DEFAULT_FRAME_TIME = 120;

static inline int min(int x, int y) {
    return x < y ? x : y;
}

void get_buffer_params(
    RenderContext_t* ctx,
    int* bytes_per_line,
    const uint8_t** start_ptr,
    int* min_y,
    int* max_y,
    int* pixels_per_byte
) {
    EpdRect area = ctx->area;

    enum EpdDrawMode mode = ctx->mode;
    const EpdRect crop_to = ctx->crop_to;
    const bool horizontally_cropped = !(crop_to.x == 0 && crop_to.width == area.width);
    const bool vertically_cropped = !(crop_to.y == 0 && crop_to.height == area.height);

    // number of pixels per byte of input data
    int width_divider = 0;

    if (mode & MODE_PACKING_1PPB_DIFFERENCE) {
        *bytes_per_line = area.width;
        width_divider = 1;
    } else if (mode & MODE_PACKING_2PPB) {
        *bytes_per_line = area.width / 2 + area.width % 2;
        width_divider = 2;
    } else if (mode & MODE_PACKING_8PPB) {
        *bytes_per_line = (area.width / 8 + (area.width % 8 > 0));
        width_divider = 8;
    } else {
        ctx->error |= EPD_DRAW_INVALID_PACKING_MODE;
    }

    int crop_x = (horizontally_cropped ? crop_to.x : 0);
    int crop_y = (vertically_cropped ? crop_to.y : 0);
    int crop_h = (vertically_cropped ? crop_to.height : 0);

    const uint8_t* ptr_start = ctx->data_ptr;

    // Adjust for negative starting coordinates with optional crop
    if (area.x - crop_x < 0) {
        ptr_start += -(area.x - crop_x) / width_divider;
    }

    if (area.y - crop_y < 0) {
        ptr_start += -(area.y - crop_y) * *bytes_per_line;
    }

    // calculate start and end row with crop
    *min_y = area.y + crop_y;
    *max_y = min(*min_y + (vertically_cropped ? crop_h : area.height), area.height);
    *start_ptr = ptr_start;
    *pixels_per_byte = width_divider;
}

void IRAM_ATTR prepare_context_for_next_frame(RenderContext_t* ctx) {
    int waveform_frame = ctx->current_frame;
    if (ctx->scroll_enabled && ctx->scroll_waveform_frames > 0 && waveform_frame >= ctx->scroll_waveform_frames) {
        waveform_frame = ctx->scroll_waveform_frames - 1;
    }

    int frame_time = DEFAULT_FRAME_TIME;
    if (ctx->phase_times != NULL) {
        if (ctx->scroll_enabled && ctx->scroll_waveform_frames > 0 && ctx->scroll_count > 0) {
            // A scroll physical frame may contain multiple bucket-local waveform
            // phases.  Drive the frame for the longest active phase; using only
            // current_frame's phase time can under-drive late buckets that are
            // still at phase 0/1 while the wavefront tail is at a later phase.
            frame_time = 0;
            for (int si = 0; si < ctx->scroll_count; ++si) {
                const int progression = ctx->scroll_direction ? (ctx->scroll_count - 1 - si) : si;
                const int phase = ctx->current_frame - progression;
                if (phase >= 0 && phase < ctx->scroll_waveform_frames) {
                    const int phase_time = ctx->phase_times[phase];
                    if (phase_time > frame_time) frame_time = phase_time;
                }
            }
            if (frame_time == 0) frame_time = ctx->phase_times[waveform_frame];
        } else {
            frame_time = ctx->phase_times[waveform_frame];
        }
    }

    if (ctx->mode & MODE_EPDIY_MONOCHROME) {
        frame_time = MONOCHROME_FRAME_TIME;
    }
    ctx->frame_time = frame_time;

    if (!(ctx->scroll_enabled && ctx->scroll_conversion_luts != NULL)) {
        const EpdWaveformPhases* phases
            = ctx->waveform->mode_data[ctx->waveform_index]->range_data[ctx->waveform_range];

        assert(ctx->lut_build_func != NULL);
        ctx->lut_build_func(ctx->conversion_lut, phases, waveform_frame);
    }

    ctx->lines_prepared = 0;
    ctx->lines_consumed = 0;
}

void epd_populate_line_mask(uint8_t* line_mask, const uint8_t* dirty_columns, int mask_len) {
    if (dirty_columns == NULL) {
        memset(line_mask, 0xFF, mask_len);
    } else {
        int pixels = mask_len * 4;
        for (int c = 0; c < pixels / 2; c += 2) {
            uint8_t mask = 0;
            mask |= (dirty_columns[c + 1] & 0xF0) != 0 ? 0xC0 : 0x00;
            mask |= (dirty_columns[c + 1] & 0x0F) != 0 ? 0x30 : 0x00;
            mask |= (dirty_columns[c] & 0xF0) != 0 ? 0x0C : 0x00;
            mask |= (dirty_columns[c] & 0x0F) != 0 ? 0x03 : 0x00;
            line_mask[c / 2] = mask;
        }
    }
}