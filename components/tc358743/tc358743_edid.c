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
    // Compressed ASCII, Big Endian (byte 8 = upper, byte 9 = lower)
    // "ESP": 'E' = 5, 'S' = 19, 'P' = 16 -> 00001 10011 10000 -> 0x1670
    out_buffer[8]  = 0x16;
    out_buffer[9]  = 0x70;
    out_buffer[10] = 0x01; // Product Code
    out_buffer[11] = 0x00;

    // Version / Revision (EDID 1.3)
    out_buffer[18] = 0x01;
    out_buffer[19] = 0x03;

    // Basic Display Parameters
    out_buffer[20] = 0x80; // Digital input
    out_buffer[21] = 0x10; // H size (16 cm)
    out_buffer[22] = 0x09; // V size (9 cm)
    out_buffer[23] = 0x78; // Gamma
    out_buffer[24] = 0x0A; // Features

    // Established Timings (e.g. 640x480)
    out_buffer[35] = 0x20;

    // Standard Timings (bytes 38-53) - Unused filled with 0x01, 0x01
    for (int i = 38; i < 54; i += 2) {
        out_buffer[i] = 0x01;
        out_buffer[i+1] = 0x01;
    }

    // Detailed Timing Descriptor (DTD) 1
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
        out_buffer[66] = 0x10; // H image size (160mm) lower 8
        out_buffer[67] = 0x09; // V image size (90mm) lower 8
        out_buffer[68] = 0x00; // image sizes upper
        out_buffer[69] = 0x00; // border
        out_buffer[70] = 0x00; // border
        out_buffer[71] = 0x1E; // flags (interlaced=0, syncs...)
    } else {
        // Fallback: 640x480@60Hz (25.175 MHz) DTD (VIC 1)
        // Total H = 640 + 160 = 800
        // Total V = 480 + 45 = 525
        // Frame rate = 25.175 MHz / (800 * 525) = 59.94 Hz
        uint16_t pix_clk = 2517; // 25.175 MHz -> 2517 (rounded to nearest 10kHz)
        out_buffer[54] = pix_clk & 0xFF;
        out_buffer[55] = pix_clk >> 8;
        out_buffer[56] = 0x80; // H active (640) lower 8
        out_buffer[57] = 0xA0; // H blanking (160) lower 8
        out_buffer[58] = 0x20; // H active upper 4 (0x2), H blanking upper 4 (0x0) -> 0x20
        out_buffer[59] = 0xE0; // V active (480) lower 8
        out_buffer[60] = 0x2D; // V blanking (45) lower 8
        out_buffer[61] = 0x10; // V active upper 4 (0x1), V blanking upper 4 (0x0) -> 0x10
        out_buffer[62] = 0x10; // H sync offset (16) lower 8
        out_buffer[63] = 0x60; // H sync width (96) lower 8
        out_buffer[64] = 0xA2; // V sync offset (10) upper 4, V sync width (2) lower 4 -> 1010 0010 -> 0xA2
        out_buffer[65] = 0x00; // sync upper bits
        out_buffer[66] = 0x10; // H image size lower 8
        out_buffer[67] = 0x09; // V image size lower 8
        out_buffer[68] = 0x00; // image sizes upper
        out_buffer[69] = 0x00; // border
        out_buffer[70] = 0x00; // border
        out_buffer[71] = 0x18; // flags: sync negative/negative -> 0x18
    }

    // Monitor Name Descriptor (Descriptor 2)
    out_buffer[72] = 0x00; out_buffer[73] = 0x00; out_buffer[74] = 0x00; out_buffer[75] = 0xFC;
    out_buffer[76] = 0x00;
    out_buffer[77] = 'E'; out_buffer[78] = 'S'; out_buffer[79] = 'P';
    out_buffer[80] = 0x0A; // newline
    for (int i = 81; i < 90; i++) out_buffer[i] = 0x20; // space padding

    // Dummy Descriptor (Descriptor 3)
    out_buffer[90] = 0x00; out_buffer[91] = 0x00; out_buffer[92] = 0x00; out_buffer[93] = 0x10;
    out_buffer[94] = 0x00;
    // Omit Monitor Range Limits as we don't have a strict analog CRT range to enforce
    for (int i = 95; i < 108; i++) out_buffer[i] = 0x00; // actually 0x0A, 0x20...
    out_buffer[95] = 0x0A;
    for (int i = 96; i < 108; i++) out_buffer[i] = 0x20;

    // Dummy Descriptor (Descriptor 4)
    out_buffer[108] = 0x00; out_buffer[109] = 0x00; out_buffer[110] = 0x00; out_buffer[111] = 0x10;
    out_buffer[112] = 0x00;
    out_buffer[113] = 0x0A;
    for (int i = 114; i < 126; i++) out_buffer[i] = 0x20;

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
