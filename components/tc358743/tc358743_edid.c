#include "tc358743_edid.h"
#include <string.h>

size_t tc358743_edid_build(uint8_t* out_buffer, size_t buffer_size, uint32_t max_pixel_clock_khz) {
    if (buffer_size < 256) return 0;

    memset(out_buffer, 0, 256);

    // --- 1. Base Block (128 bytes) ---
    out_buffer[0] = 0x00;
    for (int i = 1; i <= 6; i++) out_buffer[i] = 0xFF;
    out_buffer[7] = 0x00;

    // Vendor / Product ID
    // "ESP" in compressed ASCII:
    // 'E' = 5, 'S' = 19, 'P' = 16
    // Binary: 00001 (5), 10011 (19), 10000 (16)
    // 00001100 11100000 -> 0x0C, 0xE0 -> Little Endian: 0xE0, 0x0C
    out_buffer[8]  = 0x70;
    out_buffer[9]  = 0x16;
    out_buffer[10] = 0x01; // Product Code
    out_buffer[11] = 0x00;

    // Version / Revision (EDID 1.3)
    out_buffer[18] = 0x01;
    out_buffer[19] = 0x03;

    // Basic Display Parameters
    out_buffer[20] = 0x80; // Digital input
    out_buffer[21] = 0x10; // H size
    out_buffer[22] = 0x09; // V size
    out_buffer[23] = 0x78; // Gamma
    out_buffer[24] = 0x0A; // Features

    // Established Timings (e.g. 640x480)
    out_buffer[35] = 0x20;

    // Detailed Timing Descriptor (DTD)
    // Using 1080p30 as the highest supported standard inside base block if allowed by max clock
    if (max_pixel_clock_khz >= 74250) {
        // 1080p30 DTD (~74.25 MHz)
        uint16_t pix_clk = 7425; // 74.25 MHz / 10kHz
        out_buffer[54] = pix_clk & 0xFF;
        out_buffer[55] = pix_clk >> 8;
        out_buffer[56] = 0x80; // H active (1920) lower 8
        out_buffer[57] = 0x18; // H blanking (280) lower 8
        out_buffer[58] = 0x71; // H active upper 4, H blanking upper 4 -> 0111 0001
        out_buffer[59] = 0x38; // V active (1080) lower 8
        out_buffer[60] = 0x2D; // V blanking (45) lower 8
        out_buffer[61] = 0x40; // V active upper 4, V blanking upper 4 -> 0100 0000
        out_buffer[62] = 0x58; // H sync offset (88) lower 8
        out_buffer[63] = 0x2C; // H sync width (44) lower 8
        out_buffer[64] = 0x45; // V sync offset (4) upper 4, V sync width (5) lower 4 -> 0100 0101
        out_buffer[65] = 0x00; // sync offsets/widths upper bits
        out_buffer[66] = 0x00; // H image size lower
        out_buffer[67] = 0x00; // V image size lower
        out_buffer[68] = 0x00; // image sizes upper
        out_buffer[69] = 0x00; // border
        out_buffer[70] = 0x00; // border
        out_buffer[71] = 0x1E; // flags (interlaced=0, syncs...)
    }

    // Monitor Name Descriptor
    out_buffer[72] = 0x00;
    out_buffer[73] = 0x00;
    out_buffer[74] = 0x00;
    out_buffer[75] = 0xFC;
    out_buffer[76] = 0x00;
    out_buffer[77] = 'E';
    out_buffer[78] = 'S';
    out_buffer[79] = 'P';
    out_buffer[80] = 0x0A; // newline
    for (int i = 81; i < 90; i++) out_buffer[i] = 0x20;

    // Number of extensions (1 CEA-861 block)
    out_buffer[126] = 0x01;

    // Calculate base block checksum
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) sum += out_buffer[i];
    out_buffer[127] = (uint8_t)(256 - sum);

    // --- 2. CEA-861 Extension Block (128 bytes) ---
    uint8_t* ext = out_buffer + 128;
    ext[0] = 0x02; // Extension Tag
    ext[1] = 0x03; // Revision 3

    uint8_t dtd_offset = 4; // Start of DTDs
    uint8_t data_block_ptr = 4;

    // --- Video Data Block (VDB) ---
    uint8_t vics[10];
    uint8_t num_vics = 0;

    // VIC 16: 1080p @ 60Hz (Pixel Clock ~148.5 MHz)
    if (max_pixel_clock_khz >= 148500) {
        vics[num_vics++] = 16;
    }
    // VIC 34: 1080p @ 30Hz (Pixel Clock ~74.25 MHz)
    if (max_pixel_clock_khz >= 74250) {
        vics[num_vics++] = 34;
    }
    // VIC 4: 720p @ 60Hz (Pixel Clock ~74.25 MHz)
    if (max_pixel_clock_khz >= 74250) {
        vics[num_vics++] = 4;
    }
    // VIC 1: 640x480 @ 60Hz (Pixel Clock ~25.175 MHz)
    vics[num_vics++] = 1;

    uint8_t vdb_len = num_vics;
    if (vdb_len > 0) {
        ext[data_block_ptr++] = (0x02 << 5) | vdb_len; // Tag 2
        for (int i = 0; i < num_vics; i++) {
            ext[data_block_ptr++] = vics[i];
        }
    }

    // --- HDMI Vendor Specific Data Block (VSDB) ---
    uint8_t vsdb_len = 5;
    ext[data_block_ptr++] = (0x03 << 5) | vsdb_len; // Tag 3
    ext[data_block_ptr++] = 0x03; // IEEE OUI (HDMI Licensing)
    ext[data_block_ptr++] = 0x0C;
    ext[data_block_ptr++] = 0x00;
    ext[data_block_ptr++] = 0x10; // Physical address (1.0.0.0)
    ext[data_block_ptr++] = 0x00;

    // Update DTD offset
    dtd_offset = data_block_ptr;
    ext[2] = dtd_offset;

    // Calculate extension block checksum
    sum = 0;
    for (int i = 0; i < 127; i++) sum += ext[i];
    ext[127] = (uint8_t)(256 - sum);

    return 256;
}
