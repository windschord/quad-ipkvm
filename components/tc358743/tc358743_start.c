#include "tc358743_start.h"
#include "tc358743_edid.h"
#include "tc358743_pll_calc.h"
#include <string.h>

#define CHIPID           0x0000
#define SYSCTL           0x0002
#define CONFCTL          0x0004
#define FIFOCTL          0x0006
#define INTSTATUS        0x0014
#define INTMASK          0x0016
#define PLLCTL0          0x0020
#define PLLCTL1          0x0022
#define CECHCLK          0x0028
#define CECLCLK          0x002A

#define CLW_CNTRL        0x0140
#define D0W_CNTRL        0x0144
#define D1W_CNTRL        0x0148
#define D2W_CNTRL        0x014C
#define D3W_CNTRL        0x0150
#define STARTCNTRL       0x0204

#define LINEINITCNT      0x0210
#define LPTXTIMECNT      0x0214
#define TCLK_HEADERCNT   0x0218
#define TCLK_TRAILCNT    0x021C
#define THS_HEADERCNT    0x0220
#define TWAKEUP          0x0224
#define TCLK_POSTCNT     0x0228
#define THS_TRAILCNT     0x022C
#define HSTXVREGCNT      0x0230

#define HSTXVREGEN       0x0234
#define TXOPTIONCNTRL    0x0238

#define CSI_CONFW        0x0500
#define CSI_START        0x0518

#define SYS_STATUS       0x8520
#define PHY_CTL0         0x8531
#define PHY_CTL1         0x8532
#define PHY_CTL2         0x8533
#define PHY_EN           0x8534
#define PHY_BIAS         0x8536
#define PHY_CSQ          0x853F
#define SYS_FREQ0        0x8540
#define SYS_FREQ1        0x8541
#define DDC_CTL          0x8543
#define HPD_CTL          0x8544
#define AVM_CTL          0x8546
#define HDMI_DET         0x8552
#define HDCP_MODE        0x8560
#define VI_MODE          0x8570
#define VOUT_SET2        0x8573
#define VOUT_SET3        0x8574
#define VI_REP           0x8576
#define VI_MUTE          0x857F
#define FH_MIN0          0x85AA
#define FH_MIN1          0x85AB
#define FH_MAX0          0x85AC
#define FH_MAX1          0x85AD
#define HV_RST           0x85AF
#define EDID_MODE        0x85C7
#define EDID_LEN1        0x85CA
#define EDID_LEN2        0x85CB

#define LOCKDET_REF0     0x8630
#define LOCKDET_REF1     0x8631
#define LOCKDET_REF2     0x8632
#define NCO_F0_MOD       0x8670

#define EDID_RAM         0x8C00

static bool update8(const tc358743_io_ops_t* ops, uint16_t reg, uint8_t mask, uint8_t val) {
    uint8_t cur;
    if (!ops->read8(ops->ctx, reg, &cur)) return false;
    return ops->write8(ops->ctx, reg, (cur & ~mask) | val);
}

static bool update16(const tc358743_io_ops_t* ops, uint16_t reg, uint16_t mask, uint16_t val) {
    uint16_t cur;
    if (!ops->read16(ops->ctx, reg, &cur)) return false;
    return ops->write16(ops->ctx, reg, (cur & ~mask) | val);
}

static bool sleep_mode(const tc358743_io_ops_t* ops, bool sleep) {
    return update16(ops, SYSCTL, 0x0001, sleep ? 0x0001 : 0);
}

static bool write_edid(const tc358743_io_ops_t* ops, tc358743_edid_profile_t profile) {
    uint8_t edid[256];
    if (tc358743_edid_build_profile(edid, sizeof(edid), profile) != 256) return false;

    if (!update8(ops, EDID_MODE, 0x03, 0x00)) return false;
    if (!ops->write8(ops->ctx, EDID_LEN1, 0x00)) return false;
    if (!ops->write8(ops->ctx, EDID_LEN2, 0x01)) return false;

    for (int i = 0; i < 256; i++) {
        if (!ops->write8(ops->ctx, EDID_RAM + i, edid[i])) return false;
    }

    if (!update8(ops, EDID_MODE, 0x03, 0x02)) return false;

    ops->delay_ms(ops->ctx, 10);
    return true;
}

bool tc358743_start_sequence(const tc358743_io_ops_t* ops, const tc358743_mode_t* mode, tc358743_edid_profile_t edid_profile, bool* out_locked) {
    if (!ops || !mode || !out_locked) return false;

    *out_locked = false;

    if (mode->lanes != 2) return false;
    if (mode->format != TC358743_FORMAT_RGB888) return false;
    if (mode->lane_rate_mbps != 972 && mode->lane_rate_mbps != 594) return false;

    if (ops->reset_gpio) {
        ops->reset_gpio(ops->ctx, true);
        ops->reset_gpio(ops->ctx, false);
        ops->delay_ms(ops->ctx, 10);
        ops->reset_gpio(ops->ctx, true);
        ops->delay_ms(ops->ctx, 200);
    }

    uint16_t chipid;
    if (!ops->read16(ops->ctx, CHIPID, &chipid)) return false;
    if ((chipid & 0xFF00) != 0x0000) return false;

    // Only update HPD_OUT0 (bit 0), leave HPD_CTL0 (bit 4) alone as per linux/p4kvm
    if (!update8(ops, HPD_CTL, 0x01, 0x00)) return false;
    ops->delay_ms(ops->ctx, 20);

    if (!update16(ops, SYSCTL, 0x0C00, 0x0C00)) return false;
    if (!update16(ops, SYSCTL, 0x0300, 0x0300)) return false;
    if (!update16(ops, SYSCTL, 0x0300, 0x0000)) return false;

    if (!sleep_mode(ops, false)) return false;
    if (!ops->write16(ops->ctx, FIFOCTL, 374)) return false;

    if (!ops->write8(ops->ctx, SYS_FREQ0, 0x8C)) return false;
    if (!ops->write8(ops->ctx, SYS_FREQ1, 0x0A)) return false;
    if (!update8(ops, PHY_CTL0, 0x02, 0x00)) return false;

    if (!ops->write8(ops->ctx, FH_MIN0, 0x0E)) return false;
    if (!ops->write8(ops->ctx, FH_MIN1, 0x01)) return false;
    if (!ops->write8(ops->ctx, FH_MAX0, 0xF6)) return false;
    if (!ops->write8(ops->ctx, FH_MAX1, 0x06)) return false;

    if (!ops->write8(ops->ctx, LOCKDET_REF0, 0xB0)) return false;
    if (!ops->write8(ops->ctx, LOCKDET_REF1, 0x1E)) return false;
    if (!ops->write8(ops->ctx, LOCKDET_REF2, 0x04)) return false;

    if (!update8(ops, NCO_F0_MOD, 0x03, 0x01)) return false;

    if (!ops->write16(ops->ctx, CECHCLK, 0x01A5)) return false;
    if (!ops->write16(ops->ctx, CECLCLK, 0x01A5)) return false;

    if (!ops->write8(ops->ctx, PHY_EN, 0x00)) return false;
    if (!ops->write8(ops->ctx, PHY_CTL1, 0x80)) return false;
    if (!update8(ops, PHY_CTL2, 0x1C, 0x00)) return false;
    if (!ops->write8(ops->ctx, PHY_BIAS, 0x40)) return false;
    if (!ops->write8(ops->ctx, PHY_CSQ, 0x0A)) return false;
    if (!ops->write8(ops->ctx, AVM_CTL, 45)) return false;
    if (!update8(ops, HDMI_DET, 0x30, 0x00)) return false;
    if (!update8(ops, HV_RST, 0x03, 0x00)) return false;
    if (!ops->write8(ops->ctx, PHY_EN, 0x01)) return false;

    if (!update8(ops, DDC_CTL, 0x03, 0x02)) return false;
    if (!update8(ops, HDCP_MODE, 0x02, 0x02)) return false;

    if (!update8(ops, VI_MODE, 0x08, 0x00)) return false;
    if (!update8(ops, VOUT_SET2, 0x03, 0x01)) return false;
    if (!ops->write8(ops->ctx, VOUT_SET3, 0x08)) return false;

    if (!write_edid(ops, edid_profile)) return false;

    if (!ops->write8(ops->ctx, VI_MUTE, 0xD0)) return false;
    if (!update16(ops, CONFCTL, 0x0003, 0x0000)) return false;

    if (!sleep_mode(ops, true)) return false;

    tc358743_pll_config_t pll_cfg;
    if (!tc358743_pll_calc(27000000, mode->lane_rate_mbps * 1000, &pll_cfg)) return false;
    uint16_t pllctl0 = (((pll_cfg.pll_prd - 1) << 12) | ((pll_cfg.pll_fbd - 1) & 0x1FF));
    uint16_t pllctl1 = (pll_cfg.frs << 10);

    if (!ops->write16(ops->ctx, PLLCTL0, pllctl0)) return false;
    if (!ops->write16(ops->ctx, PLLCTL1, pllctl1 | 0x03)) return false;
    ops->delay_ms(ops->ctx, 1);
    if (!ops->write16(ops->ctx, PLLCTL1, pllctl1 | 0x13)) return false;
    if (!sleep_mode(ops, false)) return false;

    if (!update16(ops, SYSCTL, 0x0200, 0x0200)) return false;
    if (!update16(ops, SYSCTL, 0x0200, 0x0000)) return false;

    if (!ops->write32(ops->ctx, D2W_CNTRL, 0x01)) return false;
    if (!ops->write32(ops->ctx, D3W_CNTRL, 0x01)) return false;

    if (mode->lane_rate_mbps == 972) {
        if (!ops->write32(ops->ctx, LINEINITCNT, 0x1B58)) return false;
        if (!ops->write32(ops->ctx, LPTXTIMECNT, 0x0007)) return false;
        if (!ops->write32(ops->ctx, TCLK_HEADERCNT, 0x2806)) return false;
        if (!ops->write32(ops->ctx, TCLK_TRAILCNT, 0x00)) return false;
        if (!ops->write32(ops->ctx, THS_HEADERCNT, 0x0806)) return false;
        if (!ops->write32(ops->ctx, TWAKEUP, 0x4268)) return false;
        if (!ops->write32(ops->ctx, TCLK_POSTCNT, 0x0008)) return false;
        if (!ops->write32(ops->ctx, THS_TRAILCNT, 0x0005)) return false;
        if (!ops->write32(ops->ctx, HSTXVREGCNT, 0)) return false;
    } else {
        if (!ops->write32(ops->ctx, LINEINITCNT, 0x0E80)) return false;
        if (!ops->write32(ops->ctx, LPTXTIMECNT, 0x0003)) return false;
        if (!ops->write32(ops->ctx, TCLK_HEADERCNT, 0x1403)) return false;
        if (!ops->write32(ops->ctx, TCLK_TRAILCNT, 0x00)) return false;
        if (!ops->write32(ops->ctx, THS_HEADERCNT, 0x0103)) return false;
        if (!ops->write32(ops->ctx, TWAKEUP, 0x4882)) return false;
        if (!ops->write32(ops->ctx, TCLK_POSTCNT, 0x0008)) return false;
        if (!ops->write32(ops->ctx, THS_TRAILCNT, 0x0002)) return false;
        if (!ops->write32(ops->ctx, HSTXVREGCNT, 0)) return false;
    }

    if (!ops->write32(ops->ctx, HSTXVREGEN, 0x07)) return false;

    if (!ops->write32(ops->ctx, TXOPTIONCNTRL, mode->continuous_clock ? 1 : 0)) return false;
    if (!ops->write32(ops->ctx, STARTCNTRL, 1)) return false;
    if (!ops->write32(ops->ctx, CSI_START, 1)) return false;

    if (!ops->write32(ops->ctx, CSI_CONFW, 0xA3008082)) return false;
    if (!ops->write32(ops->ctx, CSI_CONFW, 0xB4000312)) return false;
    if (!ops->write32(ops->ctx, CSI_CONFW, 0xD5000012)) return false;
    if (!ops->write32(ops->ctx, CSI_CONFW, 0xA6000004)) return false;

    if (!update8(ops, VOUT_SET2, 0xC0, 0x00)) return false;
    if (!update8(ops, VI_REP, 0xE0, 0x00)) return false;
    if (!update16(ops, CONFCTL, 0x00C0, 0x0000)) return false;

    if (!ops->write16(ops->ctx, INTSTATUS, 0xFFFF)) return false;
    if (!ops->write16(ops->ctx, INTMASK, 0xFCFF)) return false;

    if (!ops->write8(ops->ctx, VI_MUTE, 0xC0)) return false;
    if (!update16(ops, CONFCTL, 0x0003, 0x0003)) return false;

    ops->delay_ms(ops->ctx, 150);

    if (!update8(ops, HPD_CTL, 0x01, 0x01)) return false;

    ops->delay_ms(ops->ctx, 50);

    if (!ops->write32(ops->ctx, CSI_START, 1)) return false;

    for (int i = 0; i < (5000 / 50); i++) {
        uint8_t status;
        if (!ops->read8(ops->ctx, SYS_STATUS, &status)) return false;

        if ((status & 0x82) == 0x82) {
            *out_locked = true;
            break;
        }
        ops->delay_ms(ops->ctx, 50);
    }

    return true;
}
