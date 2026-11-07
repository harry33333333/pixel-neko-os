/*
 * C-Media CMI8738 / CMI8338 PCI audio driver (*.pdv module).
 *
 * Register definitions and AC'97 mixer programming are based on the public
 * C-Media CMI8738/C3DX technical documentation and the Intel AC'97
 * Component Specification.
 */

#include <pdv/ddk.h>

static const pdv_kernel_api_t* g_kapi = 0;

#define klog_info(fmt, ...) g_kapi->log_printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define global_tick         (g_kapi->timer_get_ticks())

#define CMI_VENDOR_ID          0x13F6
#define CMI_DEV_8738           0x0111
#define CMI_DEV_8338A          0x0100
#define CMI_DEV_8338B          0x0101

#define CMI_REG_FUNCTRL0       0x00
#define CMI_REG_FUNCTRL1       0x04
#define CMI_REG_CHFORMAT       0x08
#define CMI_REG_INT_HLDCLR     0x0C
#define CMI_REG_INT_STATUS     0x10
#define CMI_REG_LEGACY_CTRL    0x14
#define CMI_REG_MISC_CTRL      0x18
#define CMI_REG_SB_DATA        0x22
#define CMI_REG_SB_INDEX       0x23
#define CMI_REG_MIXER1         0x24
#define CMI_REG_MIXER2         0x25
#define CMI_REG_CH0_FRAME1     0x80
#define CMI_REG_CH0_FRAME2     0x84
#define CMI_REG_EXT_MISC       0x90

#define CMI_CHADC0             0x00000001
#define CMI_CHADC1             0x00000002
#define CMI_PAUSE0             0x00000004
#define CMI_PAUSE1             0x00000008
#define CMI_CHEN0              0x00010000
#define CMI_CHEN1              0x00020000
#define CMI_RST_CH0            0x00040000
#define CMI_RST_CH1            0x00080000

#define CMI_BREQ               0x00000010

#define CMI_MISC_PWD           0x80000000
#define CMI_MISC_RESET         0x40000000
#define CMI_MISC_TXVX          0x08000000
#define CMI_MISC_N4SPK3D       0x04000000
#define CMI_MISC_ENDBDAC       0x00800000
#define CMI_MISC_XCHGDAC       0x00400000

#define CMI_EXT_MISC_ROUTING   0x00000920

#define CMI_LEGACY_DAC_EXCHANGE 0x80000000
#define CMI_CHFORMAT_SETLAT48   0x00800000

#define CMI_EXT_MIXER_SPEAKER_MUTE 0x08
#define CMI_EXT_MIXER_PHONE_MUTE   0x10

#define CMI_MIXER1_FMMUTE      0x80
#define CMI_MIXER1_WSMUTE      0x40

#define CMI_DMA_BUFFER_BYTES   65536
#define CMI_DMA_HALF_BYTES     32768
#define CMI_DMA_TOTAL_FRAMES   16384
#define CMI_DMA_HALF_FRAMES    8192

static int g_cmi_present = 0;
static unsigned short g_cmi_io_base = 0;
static unsigned short g_cmi_device = 0;
static const char* g_cmi_chip_name = "None";
static unsigned char* g_dma_buf = 0;
static unsigned int g_dma_phys = 0;
static pdv_dma_buffer_t g_dma;
static int g_dma_running = 0;
static int g_last_half_playing = -1;
static int g_half_filled[2] = {0, 0};
static int g_last_toggle_tick = 0;
static unsigned int g_last_seen_frame = 0xFFFFFFFFU;
static int g_hw_reg_moving = 0;

static inline unsigned int cmi_read32(unsigned int reg) {
    if (!g_cmi_io_base) return 0;
    return pdv_inl((uint16_t)(g_cmi_io_base + reg));
}

static inline void cmi_write32(unsigned int reg, unsigned int val) {
    if (!g_cmi_io_base) return;
    pdv_outl((uint16_t)(g_cmi_io_base + reg), val);
}

static inline unsigned short cmi_read16(unsigned int reg) {
    if (!g_cmi_io_base) return 0;
    return pdv_inw((uint16_t)(g_cmi_io_base + reg));
}

static inline void cmi_io_wait(void) {
    pdv_outb(0x80, 0);
}

static inline void cmi_write_mixer(unsigned char idx, unsigned char val) {
    if (!g_cmi_io_base) return;
    pdv_outb((uint16_t)(g_cmi_io_base + CMI_REG_SB_INDEX), idx);
    cmi_io_wait();
    pdv_outb((uint16_t)(g_cmi_io_base + CMI_REG_SB_DATA), val);
    cmi_io_wait();
}

static inline unsigned char cmi_read_mixer(unsigned char idx) {
    if (!g_cmi_io_base) return 0;
    pdv_outb((uint16_t)(g_cmi_io_base + CMI_REG_SB_INDEX), idx);
    cmi_io_wait();
    return pdv_inb((uint16_t)(g_cmi_io_base + CMI_REG_SB_DATA));
}

static void cmi8738_set_volume(int master_vol, int pcm_vol);

static int cmi8738_init(void) {
    g_cmi_present = 0;
    g_cmi_io_base = 0;
    g_cmi_device = 0;
    g_cmi_chip_name = "None";
    g_dma_running = 0;

    int count = g_kapi->pci_get_device_count();
    for (int index = 0; index < count; index++) {
        const pdv_pci_device_t* pci = g_kapi->pci_get_device(index);
        if (!pci || pci->vendor != CMI_VENDOR_ID) continue;
        unsigned short device = pci->device;

        if (device == CMI_DEV_8738) {
            g_cmi_chip_name = "C-Media CMI8738 PCI";
        } else if (device == CMI_DEV_8338A) {
            g_cmi_chip_name = "C-Media CMI8338A PCI";
        } else if (device == CMI_DEV_8338B) {
            g_cmi_chip_name = "C-Media CMI8338B PCI";
        } else {
            continue;
        }

        if (!pci->bar[0].valid || !pci->bar[0].is_io) continue;

        g_cmi_io_base = (unsigned short)pci->bar[0].address;
        g_cmi_device = device;

        g_kapi->pci_enable_io(pci);
        g_kapi->pci_enable_memory(pci);
        g_kapi->pci_enable_bus_master(pci);

        unsigned int lat = g_kapi->pci_config_read(pci->bus, pci->slot,
                                                   pci->function, 0x0C);
        if ((lat & 0xFF00U) < 0x2000U) {
            g_kapi->pci_config_write(pci->bus, pci->slot, pci->function,
                                     0x0C, (lat & 0xFFFF00FFU) | 0x4000U);
        }

        g_cmi_present = 1;
        break;
    }

    if (!g_cmi_present || !g_cmi_io_base) {
        return -1;
    }

    if (!pdv_dma_is_valid(&g_dma)) {
        g_dma = pdv_dma_alloc_aligned(g_kapi, CMI_DMA_BUFFER_BYTES, 65536);
        if (!pdv_dma_is_valid(&g_dma)) {
            g_cmi_present = 0;
            return -2;
        }
        g_dma_buf = (unsigned char*)g_dma.virt;
        g_dma_phys = (unsigned int)g_dma.phys;
    }

    for (int i = 0; i < CMI_DMA_BUFFER_BYTES; i++) {
        g_dma_buf[i] = 0;
    }
    g_half_filled[0] = 0;
    g_half_filled[1] = 0;

    cmi_write32(CMI_REG_MISC_CTRL, CMI_MISC_RESET);
    for (volatile int d = 0; d < 2000; d++);
    cmi_write32(CMI_REG_MISC_CTRL, 0);
    for (volatile int d = 0; d < 2000; d++);

    cmi_write32(CMI_REG_INT_HLDCLR, 0);

    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 |= CMI_RST_CH0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 2000; d++);
    f0 &= ~CMI_RST_CH0;
    f0 &= ~CMI_CHADC0;
    f0 |= CMI_CHADC1;
    f0 &= ~CMI_PAUSE0;
    f0 &= ~CMI_CHEN0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    unsigned int misc = cmi_read32(CMI_REG_MISC_CTRL);
    misc &= ~(CMI_MISC_PWD | CMI_MISC_RESET | CMI_MISC_XCHGDAC);
    misc |= (CMI_MISC_ENDBDAC | CMI_MISC_N4SPK3D);
    if (g_cmi_device == CMI_DEV_8738) misc |= CMI_MISC_TXVX;
    cmi_write32(CMI_REG_MISC_CTRL, misc);

    if (g_cmi_device == CMI_DEV_8338A || g_cmi_device == CMI_DEV_8338B) {
        cmi_write32(CMI_REG_EXT_MISC, 0);
    } else {
        cmi_write32(CMI_REG_EXT_MISC, CMI_EXT_MISC_ROUTING);
    }

    unsigned int leg = cmi_read32(CMI_REG_LEGACY_CTRL);
    leg &= ~CMI_LEGACY_DAC_EXCHANGE;
    cmi_write32(CMI_REG_LEGACY_CTRL, leg);

    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    f1 &= ~((7U << 10) | (7U << 13) | 0x000003C0U);
    f1 |= ((3U << 10) | (3U << 13));
    f1 |= CMI_BREQ;
    cmi_write32(CMI_REG_FUNCTRL1, f1);

    unsigned int fmt = cmi_read32(CMI_REG_CHFORMAT);
    fmt &= ~0x03U;
    fmt |= 0x03U;
    fmt |= CMI_CHFORMAT_SETLAT48;
    cmi_write32(CMI_REG_CHFORMAT, fmt);

    pdv_outb((uint16_t)(g_cmi_io_base + CMI_REG_MIXER1), 0x00);
    pdv_outb((uint16_t)(g_cmi_io_base + CMI_REG_MIXER2), 0x00);

    cmi_write_mixer(0x00, 0x00);
    cmi_write_mixer(0x3C, 0x1F);

    unsigned char ext = cmi_read_mixer(0xF0);
    ext &= (unsigned char)~(CMI_EXT_MIXER_SPEAKER_MUTE | CMI_EXT_MIXER_PHONE_MUTE);
    cmi_write_mixer(0xF0, ext);

    cmi_write32(CMI_REG_CH0_FRAME1, g_dma_phys);
    cmi_write32(CMI_REG_CH0_FRAME2, ((unsigned int)0x1FFFU << 16) | 0x3FFFU);

    cmi8738_set_volume(90, 90);
    klog_info("[CMI ] active %s io=0x%x dma=0x%x",
              g_cmi_chip_name, g_cmi_io_base, g_dma_phys);
    return 0;
}

static int cmi8738_is_present(void) {
    return g_cmi_present;
}

static const char* cmi8738_get_name(void) {
    return g_cmi_chip_name;
}

static void cmi8738_set_format(int rate, int bits, int stereo) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    unsigned int asfc = 3;
    if (rate >= 48000)      asfc = 7;
    else if (rate >= 44100) asfc = 3;
    else if (rate >= 32000) asfc = 6;
    else if (rate >= 22050) asfc = 2;
    else if (rate >= 16000) asfc = 5;
    else if (rate >= 11025) asfc = 1;
    else if (rate >= 8000)  asfc = 4;
    else                    asfc = 0;

    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    f1 &= ~((7U << 10) | (7U << 13));
    f1 |= ((asfc << 10) | (asfc << 13));
    f1 |= CMI_BREQ;
    cmi_write32(CMI_REG_FUNCTRL1, f1);

    unsigned int chfmt = 0;
    if (bits == 16) chfmt |= 0x02U;
    if (stereo)     chfmt |= 0x01U;

    unsigned int fmt = cmi_read32(CMI_REG_CHFORMAT);
    fmt &= ~0x03U;
    fmt |= chfmt;
    fmt |= CMI_CHFORMAT_SETLAT48;
    cmi_write32(CMI_REG_CHFORMAT, fmt);
}

static void cmi8738_start_dma(void) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 |= CMI_RST_CH0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 500; d++);
    f0 &= ~CMI_RST_CH0;
    f0 &= ~(CMI_CHADC0 | CMI_PAUSE0);
    cmi_write32(CMI_REG_FUNCTRL0, f0);
    for (volatile int d = 0; d < 500; d++);

    cmi_write32(CMI_REG_CH0_FRAME1, g_dma_phys);
    cmi_write32(CMI_REG_CH0_FRAME2, ((unsigned int)0x1FFFU << 16) | 0x3FFFU);

    unsigned int f1 = cmi_read32(CMI_REG_FUNCTRL1);
    if (!(f1 & CMI_BREQ)) {
        cmi_write32(CMI_REG_FUNCTRL1, f1 | CMI_BREQ);
    }

    unsigned char m1 = pdv_inb((uint16_t)(g_cmi_io_base + CMI_REG_MIXER1));
    if (m1 & CMI_MIXER1_WSMUTE) {
        pdv_outb((uint16_t)(g_cmi_io_base + CMI_REG_MIXER1),
                 (uint8_t)(m1 & ~CMI_MIXER1_WSMUTE));
    }

    f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 &= ~(CMI_CHADC0 | CMI_PAUSE0);
    f0 |= CMI_CHEN0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    g_dma_running = 1;
    g_last_half_playing = 0;
    g_last_toggle_tick = global_tick;
    g_last_seen_frame = 0xFFFFFFFFU;
    g_hw_reg_moving = 0;
}

static void cmi8738_stop_dma(void) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    f0 &= ~CMI_CHEN0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);

    cmi_write32(CMI_REG_FUNCTRL0, f0 | CMI_RST_CH0);
    for (volatile int d = 0; d < 500; d++);
    cmi_write32(CMI_REG_FUNCTRL0, f0 & ~CMI_RST_CH0);

    g_dma_running = 0;
    g_last_half_playing = -1;
    g_last_toggle_tick = 0;
    g_last_seen_frame = 0xFFFFFFFFU;
    g_hw_reg_moving = 0;
    g_half_filled[0] = 0;
    g_half_filled[1] = 0;

    for (int i = 0; i < CMI_DMA_BUFFER_BYTES; i++) {
        g_dma_buf[i] = 0;
    }
}

static void cmi8738_pause_dma(int pause) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    unsigned int f0 = cmi_read32(CMI_REG_FUNCTRL0);
    if (pause) f0 |= CMI_PAUSE0;
    else       f0 &= ~CMI_PAUSE0;
    cmi_write32(CMI_REG_FUNCTRL0, f0);
}

static void cmi8738_set_volume(int master_vol, int pcm_vol) {
    if (!g_cmi_present || !g_cmi_io_base) return;

    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    unsigned char m1 = pdv_inb((uint16_t)(g_cmi_io_base + CMI_REG_MIXER1));
    m1 &= (unsigned char)~CMI_MIXER1_WSMUTE;
    pdv_outb((uint16_t)(g_cmi_io_base + CMI_REG_MIXER1), m1);

    unsigned char m5 = (unsigned char)((master_vol * 31) / 100);
    unsigned char p5 = (unsigned char)((pcm_vol * 31) / 100);

    cmi_write_mixer(0x22, (unsigned char)(m5 << 3));
    cmi_write_mixer(0x23, (unsigned char)(m5 << 3));
    cmi_write_mixer(0x04, (unsigned char)(p5 << 3));
    cmi_write_mixer(0x05, (unsigned char)(p5 << 3));
    cmi_write_mixer(0x3C, 0x1F);

    unsigned char ext = cmi_read_mixer(0xF0);
    ext &= (unsigned char)~(CMI_EXT_MIXER_SPEAKER_MUTE | CMI_EXT_MIXER_PHONE_MUTE);
    cmi_write_mixer(0xF0, ext);
}

static unsigned int cmi8738_get_dma_frame_pos(void) {
    if (!g_cmi_present || !g_cmi_io_base || !g_dma_running) return 0;

    unsigned short rem = 0;
    for (int tries = 0; tries < 3; tries++) {
        rem = cmi_read16(CMI_REG_CH0_FRAME2);
        if (rem < CMI_DMA_TOTAL_FRAMES) break;
    }
    if (rem >= CMI_DMA_TOTAL_FRAMES) rem = CMI_DMA_TOTAL_FRAMES - 1;
    unsigned int pos =
        (CMI_DMA_TOTAL_FRAMES - (rem + 1U)) & (CMI_DMA_TOTAL_FRAMES - 1U);
    return pos;
}

static const void* cmi8738_get_dma_buffer(void) {
    return g_dma_buf;
}

static void cmi8738_write_half(int half_idx, const void* data, unsigned int bytes) {
    if (!g_dma_buf) return;
    if (half_idx < 0 || half_idx > 1 || !data) return;
    if (bytes > CMI_DMA_HALF_BYTES) bytes = CMI_DMA_HALF_BYTES;

    unsigned char* dst = g_dma_buf + (half_idx * CMI_DMA_HALF_BYTES);
    const unsigned char* src = (const unsigned char*)data;

    for (unsigned int i = 0; i < bytes; i++) {
        dst[i] = src[i];
    }
    for (unsigned int i = bytes; i < CMI_DMA_HALF_BYTES; i++) {
        dst[i] = 0;
    }
    pdv_dma_flush(g_kapi, &g_dma);
    g_half_filled[half_idx] = 1;
}

static void cmi8738_silence_half(int half_idx) {
    if (!g_dma_buf) return;
    if (half_idx < 0 || half_idx > 1) return;

    unsigned char* dst = g_dma_buf + (half_idx * CMI_DMA_HALF_BYTES);
    for (unsigned int i = 0; i < CMI_DMA_HALF_BYTES; i++) {
        dst[i] = 0;
    }
    pdv_dma_flush(g_kapi, &g_dma);
    g_half_filled[half_idx] = 1;
}

static int cmi8738_poll_half_needed(void) {
    if (!g_cmi_present || !g_cmi_io_base || !g_dma_running) return -1;

    unsigned int frame = cmi8738_get_dma_frame_pos();

    if (g_last_seen_frame == 0xFFFFFFFFU) {
        g_last_seen_frame = frame;
    } else if (frame != g_last_seen_frame) {
        g_last_seen_frame = frame;
        g_hw_reg_moving = 1;
    }

    int current_playing_half = (frame < CMI_DMA_HALF_FRAMES) ? 0 : 1;

    if (g_last_half_playing < 0) {
        g_last_half_playing = current_playing_half;
        g_last_toggle_tick = global_tick;
    }

    if (g_hw_reg_moving) {
        if (current_playing_half != g_last_half_playing) {
            g_last_half_playing = current_playing_half;
            g_last_toggle_tick = global_tick;
            int needed_half = 1 - current_playing_half;
            g_half_filled[needed_half] = 0;
            return needed_half;
        }

        int other_half = 1 - current_playing_half;
        if (!g_half_filled[other_half]) {
            return other_half;
        }
        return -1;
    }

    if (g_last_toggle_tick == 0) g_last_toggle_tick = global_tick;
    if ((global_tick - g_last_toggle_tick) >= 19) {
        g_last_toggle_tick = global_tick;
        int needed = g_last_half_playing;
        g_last_half_playing = 1 - g_last_half_playing;
        g_half_filled[needed] = 0;
        return needed;
    }

    if (!g_half_filled[0]) return 0;
    if (!g_half_filled[1]) return 1;

    return -1;
}

static int cmi8738_probe_entry(void) {
    return (cmi8738_init() == 0 && cmi8738_is_present()) ? 1 : 0;
}

static const pdv_audio_driver_t g_cmi8738_drv = {
    .name              = "C-Media CMI8738",
    .priority          = 90,
    .probe             = cmi8738_probe_entry,
    .get_name          = cmi8738_get_name,
    .is_present        = cmi8738_is_present,
    .set_format        = cmi8738_set_format,
    .start_dma         = cmi8738_start_dma,
    .stop_dma          = cmi8738_stop_dma,
    .pause_dma         = cmi8738_pause_dma,
    .set_volume        = cmi8738_set_volume,
    .poll_half_needed  = cmi8738_poll_half_needed,
    .write_half        = cmi8738_write_half,
    .silence_half      = cmi8738_silence_half,
    .get_dma_frame_pos = cmi8738_get_dma_frame_pos,
    .get_dma_buffer    = cmi8738_get_dma_buffer,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_audio_driver(&g_cmi8738_drv);
}
