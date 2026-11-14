/*
 * Ensoniq ES1371 / Creative CT4810 / CT5880 PCI audio driver (*.pdv module).
 *
 * Register definitions, sample-rate converter sequencing, and AC'97 access
 * follow the public Ensoniq ES1371 hardware documentation, Creative CT5880
 * documentation, and the Intel AC'97 Component Specification.
 */

#include <pdv/ddk.h>

static const pdv_kernel_api_t* g_kapi = 0;

#define klog_info(fmt, ...) g_kapi->log_printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define global_tick         (g_kapi->timer_get_ticks())

#define CT_VENDOR_ENSONIQ       0x1274
#define CT_DEV_ES1371           0x1371
#define CT_DEV_ES1371_COMPAT    0x0371
#define CT_DEV_CT5880           0x5880
#define CT_VENDOR_CREATIVE      0x1102
#define CT_DEV_EV1938           0x8938

#define CT_REG_CONTROL          0x00
#define CT_REG_STATUS           0x04
#define CT_REG_MEMORY_PAGE      0x0C
#define CT_REG_SRC_CONTROL      0x10
#define CT_REG_CODEC_PORT       0x14
#define CT_REG_LEGACY_CONTROL   0x18
#define CT_REG_SERIAL_CONTROL   0x20
#define CT_REG_DAC2_COUNT       0x28

#define CT_PAGE_DAC             0x0C
#define CT_REG_DAC2_FRAME       0x38
#define CT_REG_DAC2_SIZE        0x3C

#define CT_CONTROL_P2_BYPASS         (1U << 30)
#define CT_CONTROL_GPIO_OUT(val)     (((unsigned int)(val) & 0x0FU) << 16)
#define CT_CONTROL_AC97_SYNC_RESET   (1U << 14)
#define CT_CONTROL_BUS_MASTER_REQ    (1U << 7) /* ES_BREQ: internal memory test mode (must keep 0) */
#define CT_CONTROL_DAC1_ENABLE       (1U << 6)
#define CT_CONTROL_DAC2_ENABLE       (1U << 5)
#define CT_CONTROL_ADC_ENABLE        (1U << 4)

#define CT_STATUS_AC97_RESET         (1U << 29)

#define CT_SERIAL_P2_END_INC(slot)   (((slot) & 0x07) << 19)
#define CT_SERIAL_P2_END_MASK        (0x07U << 19)
#define CT_SERIAL_P2_START_INC(slot) (((slot) & 0x07) << 16)
#define CT_SERIAL_P2_START_MASK      (0x07U << 16)
#define CT_SERIAL_P2_LOOPBACK        (1U << 14)
#define CT_SERIAL_P2_PAUSE           (1U << 12)
#define CT_SERIAL_P2_INTERRUPT       (1U << 9)
#define CT_SERIAL_P2_DAC_SLOT_ENABLE (1U << 6)
#define CT_SERIAL_P2_MODE(mode)      (((mode) & 0x03) << 2)
#define CT_SERIAL_P2_MODE_MASK       (0x03U << 2)

#define CT_SRC_RAM_ADDRESS_MASK      (0x7FU << 25)
#define CT_SRC_RAM_ADDRESS(reg)      (((reg) & 0x7FU) << 25)
#define CT_SRC_RAM_WRITE_ENABLE      (1U << 24)
#define CT_SRC_RAM_BUSY              (1U << 23)
#define CT_SRC_DISABLE               (1U << 22)
#define CT_SRC_DAC1_DISABLE          (1U << 21)
#define CT_SRC_DAC2_DISABLE          (1U << 20)
#define CT_SRC_ADC_DISABLE           (1U << 19)

#define CT_SRC_DAC1_BASE             0x70
#define CT_SRC_DAC2_BASE             0x74
#define CT_SRC_ADC_BASE              0x78
#define CT_SRC_ADC_VOLUME_BASE       0x6C
#define CT_SRC_DAC1_VOLUME_BASE      0x7C
#define CT_SRC_DAC2_VOLUME_BASE      0x7E
#define CT_SRC_TRUNCATION            0x00
#define CT_SRC_INTEGER_FREQUENCY     0x01
#define CT_SRC_FRACTIONAL_FREQUENCY  0x03

#define CT_CODEC_READY               (1U << 31)
#define CT_CODEC_WRITE_BUSY          (1U << 30)
#define CT_CODEC_READ_REQUEST        (1U << 23)
#define CT_EV1938_CODEC_ACCESS       (1U << 26)
#define CT_AC97_RESET                0x00
#define CT_AC97_MASTER_VOLUME        0x02
#define CT_AC97_HEADPHONE_VOLUME     0x04
#define CT_AC97_MASTER_MONO_VOLUME   0x06
#define CT_AC97_PCM_OUT_VOLUME       0x18
#define CT_AC97_GENERAL_PURPOSE      0x20
#define CT_AC97_POWERDOWN            0x26
#define CT_AC97_EXT_AUDIO_ID         0x28
#define CT_AC97_EXT_AUDIO_CTRL       0x2A
#define CT_AC97_PCM_FRONT_DAC_RATE   0x2C
#define CT_AC97_VENDOR_ID1           0x7C
#define CT_AC97_VENDOR_ID2           0x7E

#define CT_DMA_BUFFER_BYTES     65536
#define CT_DMA_HALF_BYTES       32768
#define CT_DMA_TOTAL_FRAMES     16384
#define CT_DMA_HALF_FRAMES      8192

#define CT_SRC_STATE_MASK \
    (CT_SRC_DISABLE | CT_SRC_DAC1_DISABLE | CT_SRC_DAC2_DISABLE | CT_SRC_ADC_DISABLE)
#define CT_SRC_CODEC_ACCESS_STATE 0x00010000U
#define CT_SRC_CODEC_STATE_MASK   0x00870000U
#define CT_WAIT_LOOPS             0x0400
#define CT_CODEC_WAIT_LOOPS       64
#define CT_SRC_TRUNCATION_OFFSET  0x00U
#define CT_SRC_ACCUM_FRAC_OFFSET  0x02U
#define CT_SRC_DAC2_VOL_LEFT_REG  0x7EU
#define CT_SRC_DAC2_VOL_RIGHT_REG 0x7FU

static int g_ct_present = 0;
static unsigned short g_ct_io_base = 0;
static const char* g_ct_chip_name = "None";
static char g_ct_display_name[72];
static unsigned char* g_ct_dma_buf = 0;
static unsigned int g_ct_dma_phys = 0;
static pdv_dma_buffer_t g_ct_dma;
static int g_ct_dma_running = 0;
static int g_ct_last_half_playing = -1;
static int g_ct_half_filled[2] = {0, 0};
static int g_ct_last_toggle_tick = 0;
static unsigned int g_ct_last_seen_frame = 0xFFFFFFFFU;
static int g_ct_hw_reg_moving = 0;
static unsigned char g_ct_revision = 0;
static int g_ct_is_ev1938 = 0;
static unsigned int g_ct_ctrl_base = 0;
static int g_ct_sample_rate = 44100;
static int g_ct_bytes_per_sample = 4;
static int g_ct_last_master_vol = -1;
static int g_ct_last_pcm_vol = -1;

static inline unsigned int ct_read32(unsigned int reg) {
    if (!g_ct_io_base) return 0;
    return pdv_inl((uint16_t)(g_ct_io_base + reg));
}

static inline void ct_write32(unsigned int reg, unsigned int val) {
    if (!g_ct_io_base) return;
    pdv_outl((uint16_t)(g_ct_io_base + reg), val);
}

static inline void ct_io_delay(void) {
    pdv_outb(0x80, 0);
}

static void ct_copy_string(char* dst, const char* src, int max_len) {
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void ct_append_string(char* dst, const char* suffix, int max_len) {
    int pos = 0;
    while (pos < max_len - 1 && dst[pos]) pos++;
    for (int i = 0; suffix[i] && pos < max_len - 1; i++) {
        dst[pos++] = suffix[i];
    }
    dst[pos] = '\0';
}

static void ct_set_codec_name(unsigned short id1, unsigned short id2) {
    ct_copy_string(g_ct_display_name, g_ct_chip_name, sizeof(g_ct_display_name));

    if (id1 == 0x414B && id2 == 0x4D00) {
        ct_append_string(g_ct_display_name, " / AK4540 AC'97",
                         sizeof(g_ct_display_name));
    } else if (id1 == 0x5452 && id2 == 0x4103) {
        ct_append_string(g_ct_display_name, " / TR28023 (CT1297) AC'97",
                         sizeof(g_ct_display_name));
    } else if (id1 == 0x8384) {
        ct_append_string(g_ct_display_name, " / SigmaTel AC'97",
                         sizeof(g_ct_display_name));
    }

    if (g_ct_display_name[0]) {
        g_ct_chip_name = g_ct_display_name;
    }
}

static unsigned int ct_src_wait_idle(void) {
    unsigned int r = 0;
    if (!g_ct_io_base) return 0;
    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        r = ct_read32(CT_REG_SRC_CONTROL);
        if (!(r & CT_SRC_RAM_BUSY)) return r;
        ct_io_delay();
    }
    return ct_read32(CT_REG_SRC_CONTROL) & CT_SRC_STATE_MASK;
}

static unsigned short ct_src_read_word(unsigned short reg) {
    unsigned int saved_state;
    unsigned int sampled_state;
    unsigned int control;

    if (!g_ct_io_base) return 0;

    saved_state = ct_src_wait_idle();
    control = (saved_state & CT_SRC_STATE_MASK) | CT_SRC_RAM_ADDRESS(reg);
    ct_write32(CT_REG_SRC_CONTROL, control);

    sampled_state = ct_src_wait_idle();
    return (unsigned short)(sampled_state & 0xFFFFU);
}

static void ct_src_write_word(unsigned short reg, unsigned short data) {
    unsigned int r = ct_src_wait_idle() & CT_SRC_STATE_MASK;
    r |= CT_SRC_RAM_ADDRESS(reg) | (unsigned int)data;
    ct_write32(CT_REG_SRC_CONTROL, r | CT_SRC_RAM_WRITE_ENABLE);
}

static unsigned int ct_codec_access_begin(void) {
    unsigned int saved = ct_src_wait_idle() & CT_SRC_STATE_MASK;
    ct_write32(CT_REG_SRC_CONTROL, saved | CT_SRC_CODEC_ACCESS_STATE);

    /* Bound AC-link state sync polling so emulated ES1371 (VMware/QEMU) does not stall. */
    for (int t = 0; t < CT_CODEC_WAIT_LOOPS; t++) {
        if ((ct_read32(CT_REG_SRC_CONTROL) & CT_SRC_CODEC_STATE_MASK) == 0) break;
        ct_io_delay();
    }
    for (int t = 0; t < CT_CODEC_WAIT_LOOPS; t++) {
        if ((ct_read32(CT_REG_SRC_CONTROL) & CT_SRC_CODEC_STATE_MASK) ==
            CT_SRC_CODEC_ACCESS_STATE) break;
        ct_io_delay();
    }
    return saved;
}

static void ct_codec_access_end(unsigned int saved) {
    ct_src_wait_idle();
    ct_write32(CT_REG_SRC_CONTROL, saved & CT_SRC_STATE_MASK);
}

static void ct_ac97_write(unsigned char reg, unsigned short val) {
    unsigned int command;
    unsigned int saved;

    if (!g_ct_io_base) return;
    command = ((unsigned int)(reg & 0x7FU) << 16) | val;
    if (g_ct_is_ev1938) command |= CT_EV1938_CODEC_ACCESS;

    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        if (!(ct_read32(CT_REG_CODEC_PORT) & CT_CODEC_WRITE_BUSY)) {
            saved = ct_codec_access_begin();
            ct_write32(CT_REG_CODEC_PORT, command);
            ct_codec_access_end(saved);
            for (int w = 0; w < CT_WAIT_LOOPS; w++) {
                if (!(ct_read32(CT_REG_CODEC_PORT) & CT_CODEC_WRITE_BUSY)) break;
                ct_io_delay();
            }
            return;
        }
        ct_io_delay();
    }
}

static unsigned short ct_ac97_read(unsigned char reg) {
    unsigned int command;
    unsigned int saved;

    if (!g_ct_io_base) return 0;
    command = ((unsigned int)(reg & 0x7FU) << 16) | CT_CODEC_READ_REQUEST;
    if (g_ct_is_ev1938) command |= CT_EV1938_CODEC_ACCESS;

    for (int retry = 0; retry < 10; retry++) {
        int started = 0;
        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            if (!(ct_read32(CT_REG_CODEC_PORT) & CT_CODEC_WRITE_BUSY)) {
                saved = ct_codec_access_begin();
                ct_write32(CT_REG_CODEC_PORT, command);
                ct_codec_access_end(saved);
                started = 1;
                break;
            }
            ct_io_delay();
        }
        if (!started) continue;

        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            if (!(ct_read32(CT_REG_CODEC_PORT) & CT_CODEC_WRITE_BUSY)) break;
            ct_io_delay();
        }
        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            unsigned int value = ct_read32(CT_REG_CODEC_PORT);
            if (value & CT_CODEC_READY) {
                if (g_ct_is_ev1938) {
                    for (int d = 0; d < 100; d++) (void)ct_read32(CT_REG_CONTROL);
                }
                return (unsigned short)(value & 0xFFFFU);
            }
            ct_io_delay();
        }
    }
    return 0;
}

static unsigned int ct_src_channel_disable_bit(unsigned short base) {
    if (base == CT_SRC_DAC2_BASE) return CT_SRC_DAC2_DISABLE;
    if (base == CT_SRC_DAC1_BASE) return CT_SRC_DAC1_DISABLE;
    return CT_SRC_ADC_DISABLE;
}

static void ct_src_set_rate(unsigned short base, int rate) {
    unsigned int disable;
    unsigned int r;
    unsigned int freq;
    unsigned short integer_field;

    if (rate < 3000) rate = 3000;
    if (rate > 48000) rate = 48000;
    freq = (((unsigned int)rate << 15) + 1500U) / 3000U;

    disable = ct_src_channel_disable_bit(base);
    r = (ct_src_wait_idle() & CT_SRC_STATE_MASK) | disable;
    ct_write32(CT_REG_SRC_CONTROL, r);

    if (base == CT_SRC_DAC2_BASE) {
        ct_src_write_word((unsigned short)(base + CT_SRC_TRUNCATION_OFFSET),
                          (unsigned short)(16U << 4));
    }
    integer_field = ct_src_read_word((unsigned short)(base + CT_SRC_INTEGER_FREQUENCY));
    ct_src_write_word((unsigned short)(base + CT_SRC_INTEGER_FREQUENCY),
                      (unsigned short)((integer_field & 0x00FFU) |
                                      ((freq >> 5) & 0xFC00U)));
    ct_src_write_word((unsigned short)(base + CT_SRC_ACCUM_FRAC_OFFSET), 0U);
    ct_src_write_word((unsigned short)(base + CT_SRC_FRACTIONAL_FREQUENCY),
                      (unsigned short)(freq & 0x7FFFU));

    if (base == CT_SRC_DAC2_BASE) {
        ct_src_write_word(CT_SRC_DAC2_VOL_LEFT_REG, 0x1000U);
        ct_src_write_word(CT_SRC_DAC2_VOL_RIGHT_REG, 0x1000U);
    }

    r = ct_src_wait_idle() & CT_SRC_STATE_MASK & ~(disable | CT_SRC_DISABLE);
    ct_write32(CT_REG_SRC_CONTROL, r);
}

typedef struct {
    unsigned short address;
    unsigned short value;
} ct_src_default_t;

static void ct_src_load_channel_defaults(void) {
    static const ct_src_default_t defaults[] = {
        { CT_SRC_DAC1_BASE + CT_SRC_TRUNCATION,            (unsigned short)(16U << 4) },
        { CT_SRC_DAC1_BASE + CT_SRC_INTEGER_FREQUENCY,     (unsigned short)(16U << 10) },
        { CT_SRC_DAC2_BASE + CT_SRC_TRUNCATION,            (unsigned short)(16U << 4) },
        { CT_SRC_DAC2_BASE + CT_SRC_INTEGER_FREQUENCY,     (unsigned short)(16U << 10) },
        { CT_SRC_ADC_BASE + CT_SRC_TRUNCATION,             (unsigned short)(16U << 4) },
        { CT_SRC_ADC_BASE + CT_SRC_INTEGER_FREQUENCY,      (unsigned short)(16U << 10) },
        { CT_SRC_ADC_VOLUME_BASE,                          (unsigned short)(1U << 12) },
        { CT_SRC_ADC_VOLUME_BASE + 1,                      (unsigned short)(1U << 12) },
        { CT_SRC_DAC1_VOLUME_BASE,                         (unsigned short)(1U << 12) },
        { CT_SRC_DAC1_VOLUME_BASE + 1,                     (unsigned short)(1U << 12) },
        { CT_SRC_DAC2_VOLUME_BASE,                         (unsigned short)(1U << 12) },
        { CT_SRC_DAC2_VOLUME_BASE + 1,                     (unsigned short)(1U << 12) }
    };

    unsigned int count = sizeof(defaults) / sizeof(defaults[0]);
    for (unsigned int i = 0; i < count; i++) {
        ct_src_write_word(defaults[i].address, defaults[i].value);
    }
}

static int ct_device_supported(unsigned short vendor, unsigned short device) {
    if (vendor == CT_VENDOR_ENSONIQ) {
        return device == CT_DEV_ES1371 ||
               device == CT_DEV_ES1371_COMPAT ||
               device == CT_DEV_CT5880;
    }
    return vendor == CT_VENDOR_CREATIVE &&
           (device == CT_DEV_CT5880 || device == CT_DEV_EV1938);
}

static int ct_needs_ac97_reset_quirk(unsigned short vendor,
                                     unsigned short device,
                                     unsigned char revision) {
    (void)vendor;
    if (device == CT_DEV_CT5880) {
        return revision == 0x02 || revision == 0x03 || revision == 0x04;
    }
    if (device == CT_DEV_ES1371 || device == CT_DEV_ES1371_COMPAT) {
        return revision == 0x07 || revision == 0x08;
    }
    return 0;
}

static void ct4810_set_format(int rate, int bits, int stereo);

static int ct4810_init(void) {
    unsigned int status = 0;
    unsigned short vendor = 0;
    unsigned short device = 0;

    g_ct_present = 0;
    g_ct_io_base = 0;
    g_ct_chip_name = "None";
    g_ct_display_name[0] = '\0';
    g_ct_dma_running = 0;
    g_ct_revision = 0;
    g_ct_is_ev1938 = 0;
    g_ct_ctrl_base = 0;

    int count = g_kapi->pci_get_device_count();
    for (int index = 0; index < count; index++) {
        const pdv_pci_device_t* pci = g_kapi->pci_get_device(index);
        if (!pci) continue;
        vendor = pci->vendor;
        device = pci->device;
        if (!ct_device_supported(vendor, device)) continue;

        for (int bar = 0; bar < 6; bar++) {
            if (pci->bar[bar].valid && pci->bar[bar].is_io) {
                g_ct_io_base = (unsigned short)pci->bar[bar].address;
                break;
            }
        }
        if (!g_ct_io_base) continue;

        g_kapi->pci_enable_io(pci);
        g_kapi->pci_enable_memory(pci);
        g_kapi->pci_enable_bus_master(pci);

        unsigned int latency = g_kapi->pci_config_read(pci->bus, pci->slot,
                                                       pci->function, 0x0C);
        if ((latency & 0xFF00U) < 0x4000U) {
            g_kapi->pci_config_write(pci->bus, pci->slot, pci->function,
                                     0x0C, (latency & 0xFFFF00FFU) | 0x4000U);
        }

        g_ct_revision = pci->revision;
        g_ct_is_ev1938 = (device == CT_DEV_EV1938);
        g_ct_ctrl_base = g_ct_is_ev1938 ? CT_CONTROL_GPIO_OUT(1U) : 0U;

        if (device == CT_DEV_CT5880) {
            g_ct_chip_name = "Creative CT4810 (CT5880) PCI";
        } else if (g_ct_is_ev1938) {
            g_ct_chip_name = "Creative AudioPCI (EV1938) PCI";
        } else if (device == CT_DEV_ES1371_COMPAT) {
            g_ct_chip_name = "Ensoniq AudioPCI (CT4810 compatible) PCI";
        } else {
            g_ct_chip_name = "Ensoniq AudioPCI (ES1371) PCI";
        }
        g_ct_present = 1;
        break;
    }

    if (!g_ct_present || !g_ct_io_base) return -1;

    if (!pdv_dma_is_valid(&g_ct_dma)) {
        g_ct_dma = pdv_dma_alloc_aligned(g_kapi, CT_DMA_BUFFER_BYTES, 65536);
        if (!pdv_dma_is_valid(&g_ct_dma)) {
            g_ct_present = 0;
            return -2;
        }
        g_ct_dma_buf = (unsigned char*)g_ct_dma.virt;
        g_ct_dma_phys = (unsigned int)g_ct_dma.phys;
    }

    for (int i = 0; i < CT_DMA_BUFFER_BYTES; i++) g_ct_dma_buf[i] = 0;
    g_ct_half_filled[0] = 0;
    g_ct_half_filled[1] = 0;

    ct_write32(CT_REG_CONTROL, g_ct_ctrl_base);
    ct_write32(CT_REG_SERIAL_CONTROL, 0);
    ct_write32(CT_REG_LEGACY_CONTROL, 0);

    if (ct_needs_ac97_reset_quirk(vendor, device, g_ct_revision)) {
        status |= CT_STATUS_AC97_RESET;
    }
    if (status) {
        ct_write32(CT_REG_STATUS, status);
        g_kapi->sleep_ms(20);
    }

    ct_write32(CT_REG_CONTROL, g_ct_ctrl_base | CT_CONTROL_AC97_SYNC_RESET);
    (void)ct_read32(CT_REG_CONTROL);
    g_kapi->sleep_ms(2);
    ct_write32(CT_REG_CONTROL, g_ct_ctrl_base);

    (void)ct_src_wait_idle();
    ct_write32(CT_REG_SRC_CONTROL, CT_SRC_DISABLE);
    for (int i = 0; i < 0x80; i++) ct_src_write_word((unsigned short)i, 0);

    ct_src_load_channel_defaults();

    ct_src_set_rate(CT_SRC_DAC1_BASE, 44100);
    ct_src_set_rate(CT_SRC_DAC2_BASE, 44100);
    ct_src_set_rate(CT_SRC_ADC_BASE, 44100);
    (void)ct_src_wait_idle();
    ct_write32(CT_REG_SRC_CONTROL, 0);

    ct_ac97_write(CT_AC97_RESET, 0x0000);
    g_kapi->sleep_ms(50);
    unsigned short id1 = ct_ac97_read(CT_AC97_VENDOR_ID1);
    unsigned short id2 = ct_ac97_read(CT_AC97_VENDOR_ID2);
    if ((id1 == 0x0000U || id1 == 0xFFFFU) && !(status & CT_STATUS_AC97_RESET)) {
        status |= CT_STATUS_AC97_RESET;
        ct_write32(CT_REG_STATUS, status);
        g_kapi->sleep_ms(20);
        ct_write32(CT_REG_CONTROL, g_ct_ctrl_base | CT_CONTROL_AC97_SYNC_RESET);
        (void)ct_read32(CT_REG_CONTROL);
        g_kapi->sleep_ms(2);
        ct_write32(CT_REG_CONTROL, g_ct_ctrl_base);
        ct_ac97_write(CT_AC97_RESET, 0x0000);
        g_kapi->sleep_ms(50);
        id1 = ct_ac97_read(CT_AC97_VENDOR_ID1);
        id2 = ct_ac97_read(CT_AC97_VENDOR_ID2);
    }
    if (id1 || id2) ct_set_codec_name(id1, id2);

    ct_ac97_write(CT_AC97_POWERDOWN, 0x0000);
    unsigned short pwr = 0;
    int pwr_polls = (id1 != 0x0000U && id1 != 0xFFFFU) ? 20 : 3;
    for (int i = 0; i < pwr_polls; i++) {
        pwr = ct_ac97_read(CT_AC97_POWERDOWN);
        if ((pwr & 0x000FU) == 0x000FU) break;
        g_kapi->sleep_ms(5);
    }

    ct_ac97_write(CT_AC97_MASTER_VOLUME, 0x0000);
    ct_ac97_write(CT_AC97_HEADPHONE_VOLUME, 0x0000);
    ct_ac97_write(CT_AC97_MASTER_MONO_VOLUME, 0x0000);
    ct_ac97_write(CT_AC97_PCM_OUT_VOLUME, 0x0000);
    ct_ac97_write(CT_AC97_GENERAL_PURPOSE, 0x0000);
    ct_ac97_write(0x0A, 0x8000);
    ct_ac97_write(0x0E, 0x8008);
    ct_ac97_write(0x10, 0x8808);
    ct_ac97_write(0x12, 0x8808);

    /* Ensure the AC-link DAC rate stays at 48 kHz (ES1371 SRC fixed output rate). */
    unsigned short ext_id = ct_ac97_read(CT_AC97_EXT_AUDIO_ID);
    if (ext_id != 0xFFFFU && (ext_id & 0x0001U)) {
        unsigned short ext_ctrl = ct_ac97_read(CT_AC97_EXT_AUDIO_CTRL);
        ct_ac97_write(CT_AC97_EXT_AUDIO_CTRL, (unsigned short)(ext_ctrl | 0x0001U));
        ct_ac97_write(CT_AC97_PCM_FRONT_DAC_RATE, 48000U);
    }

    ct4810_set_format(44100, 16, 1);

    ct_write32(CT_REG_MEMORY_PAGE, CT_PAGE_DAC);
    ct_write32(CT_REG_DAC2_FRAME, g_ct_dma_phys);
    ct_write32(CT_REG_DAC2_SIZE, (CT_DMA_BUFFER_BYTES / 4) - 1);
    ct_write32(CT_REG_DAC2_COUNT, (CT_DMA_HALF_BYTES / 4) - 1);

    klog_info("[CT48] active %s io=0x%x dma=0x%x codec=%04x:%04x pwr=0x%x",
              g_ct_chip_name, g_ct_io_base, g_ct_dma_phys, id1, id2, pwr);
    return 0;
}

static int ct4810_is_present(void) {
    return g_ct_present;
}

static const char* ct4810_get_name(void) {
    return g_ct_chip_name;
}

static void ct4810_set_format(int rate, int bits, int stereo) {
    if (!g_ct_present || !g_ct_io_base) return;

    if (rate < 3000) rate = 3000;
    if (rate > 48000) rate = 48000;
    g_ct_sample_rate = rate;
    g_ct_bytes_per_sample = ((bits == 16) ? 2 : 1) * (stereo ? 2 : 1);

    unsigned int mode = 0;
    if (bits == 16) mode |= 0x02U;
    if (stereo) mode |= 0x01U;

    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    sctrl &= ~(CT_SERIAL_P2_LOOPBACK | CT_SERIAL_P2_PAUSE | CT_SERIAL_P2_INTERRUPT |
               CT_SERIAL_P2_DAC_SLOT_ENABLE | CT_SERIAL_P2_END_MASK |
               CT_SERIAL_P2_START_MASK | CT_SERIAL_P2_MODE_MASK);
    sctrl |= CT_SERIAL_P2_MODE(mode) |
             CT_SERIAL_P2_END_INC(mode & 0x02U ? 2U : 1U) | CT_SERIAL_P2_START_INC(0);
    ct_write32(CT_REG_SERIAL_CONTROL, sctrl);

    ct_src_set_rate(CT_SRC_DAC2_BASE, rate);
}

static void ct4810_start_dma(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_buf) return;

    pdv_dma_flush(g_kapi, &g_ct_dma);

    /* Ensure DAC2_ENABLE performs a clean 0 -> 1 edge after DMA registers are armed. */
    /* Never set bit 7 (ES_BREQ): it enables internal test mode and halts DMA. */
    unsigned int control = ct_read32(CT_REG_CONTROL);
    control &= ~(CT_CONTROL_DAC2_ENABLE | CT_CONTROL_P2_BYPASS | CT_CONTROL_BUS_MASTER_REQ);
    control |= g_ct_ctrl_base;
    ct_write32(CT_REG_CONTROL, control);

    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    sctrl &= ~(CT_SERIAL_P2_LOOPBACK | CT_SERIAL_P2_INTERRUPT |
               CT_SERIAL_P2_DAC_SLOT_ENABLE);
    sctrl |= CT_SERIAL_P2_PAUSE;
    ct_write32(CT_REG_SERIAL_CONTROL, sctrl);

    ct_write32(CT_REG_MEMORY_PAGE, CT_PAGE_DAC);
    ct_write32(CT_REG_DAC2_FRAME, g_ct_dma_phys);
    ct_write32(CT_REG_DAC2_SIZE, (CT_DMA_BUFFER_BYTES / 4) - 1);
    ct_write32(CT_REG_DAC2_COUNT, (CT_DMA_HALF_BYTES / 4) - 1);
    (void)ct_read32(CT_REG_DAC2_FRAME);

    control |= CT_CONTROL_DAC2_ENABLE;
    ct_write32(CT_REG_CONTROL, control);
    (void)ct_read32(CT_REG_CONTROL);

    /* Transition P2_PAUSE 1 -> 0 while DAC2_ENABLE=1 so VMware ES1371 starts fetching. */
    sctrl &= ~CT_SERIAL_P2_PAUSE;
    ct_write32(CT_REG_SERIAL_CONTROL, sctrl);

    g_ct_dma_running = 1;
    g_ct_last_half_playing = 0;
    g_ct_last_toggle_tick = global_tick;
    g_ct_last_seen_frame = 0xFFFFFFFFU;
    g_ct_hw_reg_moving = 0;
}

static void ct4810_stop_dma(void) {
    if (!g_ct_present || !g_ct_io_base) return;

    unsigned int control = ct_read32(CT_REG_CONTROL);
    control &= ~(CT_CONTROL_BUS_MASTER_REQ | CT_CONTROL_DAC2_ENABLE);
    control |= g_ct_ctrl_base;
    ct_write32(CT_REG_CONTROL, control);

    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    ct_write32(CT_REG_SERIAL_CONTROL, (sctrl & ~CT_SERIAL_P2_INTERRUPT) | CT_SERIAL_P2_PAUSE);

    g_ct_dma_running = 0;
    g_ct_last_half_playing = -1;
    g_ct_last_toggle_tick = 0;
    g_ct_last_seen_frame = 0xFFFFFFFFU;
    g_ct_hw_reg_moving = 0;
    g_ct_half_filled[0] = 0;
    g_ct_half_filled[1] = 0;

    if (g_ct_dma_buf) {
        for (int i = 0; i < CT_DMA_BUFFER_BYTES; i++) g_ct_dma_buf[i] = 0;
    }
}

static unsigned int ct4810_get_dma_frame_pos(void);

static void ct4810_pause_dma(int pause) {
    if (!g_ct_present || !g_ct_io_base) return;
    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    if (pause) {
        sctrl |= CT_SERIAL_P2_PAUSE;
        ct_write32(CT_REG_SERIAL_CONTROL, sctrl);
    } else {
        unsigned int frame = ct4810_get_dma_frame_pos();
        g_ct_last_seen_frame = frame;
        g_ct_last_half_playing = (frame < CT_DMA_HALF_FRAMES) ? 0 : 1;
        g_ct_last_toggle_tick = global_tick;
        sctrl &= ~CT_SERIAL_P2_PAUSE;
        ct_write32(CT_REG_SERIAL_CONTROL, sctrl);
    }
}

static void ct4810_set_volume(int master_vol, int pcm_vol) {
    if (!g_ct_present || !g_ct_io_base) return;

    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    if (master_vol == g_ct_last_master_vol && pcm_vol == g_ct_last_pcm_vol) {
        return;
    }

    unsigned char m_att = (unsigned char)((100 - master_vol) * 31 / 100);
    unsigned char p_att = (unsigned char)((100 - pcm_vol) * 31 / 100);
    unsigned short m_val =
        (master_vol == 0) ? 0x8000U : (unsigned short)((m_att << 8) | m_att);
    unsigned short p_val =
        (pcm_vol == 0) ? 0x8000U : (unsigned short)((p_att << 8) | p_att);

    if (master_vol != g_ct_last_master_vol) {
        ct_ac97_write(CT_AC97_MASTER_VOLUME, m_val);
        ct_ac97_write(CT_AC97_HEADPHONE_VOLUME, m_val);
        g_ct_last_master_vol = master_vol;
    }
    if (pcm_vol != g_ct_last_pcm_vol) {
        ct_ac97_write(CT_AC97_PCM_OUT_VOLUME, p_val);
        g_ct_last_pcm_vol = pcm_vol;
    }
}

static unsigned int ct4810_get_dma_frame_pos(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_running) return 0;

    ct_write32(CT_REG_MEMORY_PAGE, CT_PAGE_DAC);
    unsigned int val = ct_read32(CT_REG_DAC2_SIZE);
    unsigned int frame = (val >> 16) & 0xFFFFU;
    if (frame >= CT_DMA_TOTAL_FRAMES) frame = 0;
    return frame;
}

static const void* ct4810_get_dma_buffer(void) {
    return g_ct_dma_buf;
}

static void ct4810_write_half(int half_idx, const void* data, unsigned int bytes) {
    if (!g_ct_dma_buf) return;
    if (half_idx < 0 || half_idx > 1 || !data) return;
    if (bytes > CT_DMA_HALF_BYTES) bytes = CT_DMA_HALF_BYTES;

    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    const unsigned char* src = (const unsigned char*)data;
    for (unsigned int i = 0; i < bytes; i++) dest[i] = src[i];
    for (unsigned int i = bytes; i < CT_DMA_HALF_BYTES; i++) dest[i] = 0;

    pdv_dma_flush(g_kapi, &g_ct_dma);
    g_ct_half_filled[half_idx] = 1;
}

static void ct4810_silence_half(int half_idx) {
    if (!g_ct_dma_buf || half_idx < 0 || half_idx > 1) return;
    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    for (unsigned int i = 0; i < CT_DMA_HALF_BYTES; i++) dest[i] = 0;
    pdv_dma_flush(g_kapi, &g_ct_dma);
    g_ct_half_filled[half_idx] = 0;
}

static int ct4810_poll_half_needed(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_running) return -1;

    unsigned int frame = ct4810_get_dma_frame_pos();
    if (g_ct_last_seen_frame == 0xFFFFFFFFU) {
        g_ct_last_seen_frame = frame;
    } else if (frame != g_ct_last_seen_frame) {
        g_ct_last_seen_frame = frame;
        g_ct_hw_reg_moving = 1;
    }

    int current_playing_half = (frame < CT_DMA_HALF_FRAMES) ? 0 : 1;
    if (g_ct_last_half_playing < 0) {
        g_ct_last_half_playing = current_playing_half;
        g_ct_last_toggle_tick = global_tick;
    }

    if (g_ct_hw_reg_moving) {
        if (current_playing_half != g_ct_last_half_playing) {
            g_ct_last_half_playing = current_playing_half;
            g_ct_last_toggle_tick = global_tick;
            int needed_half = 1 - current_playing_half;
            g_ct_half_filled[needed_half] = 0;
            return needed_half;
        }

        int other_half = 1 - current_playing_half;
        if (!g_ct_half_filled[other_half]) return other_half;
        return -1;
    }

    int bytes_per_sec = g_ct_sample_rate * g_ct_bytes_per_sample;
    if (bytes_per_sec <= 0) bytes_per_sec = 176400;
    int half_ticks = (CT_DMA_HALF_BYTES * 100 + bytes_per_sec / 2) / bytes_per_sec;
    if (half_ticks < 2) half_ticks = 2;

    if (g_ct_last_toggle_tick == 0) g_ct_last_toggle_tick = global_tick;
    if ((global_tick - g_ct_last_toggle_tick) >= half_ticks) {
        g_ct_last_toggle_tick += half_ticks;
        if ((global_tick - g_ct_last_toggle_tick) > half_ticks * 4) {
            g_ct_last_toggle_tick = global_tick;
        }
        int needed = g_ct_last_half_playing;
        g_ct_last_half_playing = 1 - g_ct_last_half_playing;
        g_ct_half_filled[needed] = 0;
        return needed;
    }

    if (!g_ct_half_filled[0]) return 0;
    if (!g_ct_half_filled[1]) return 1;
    return -1;
}

static int ct4810_probe_entry(void) {
    return (ct4810_init() == 0 && ct4810_is_present()) ? 1 : 0;
}

static const pdv_audio_driver_t g_ct4810_drv = {
    .name              = "Ensoniq AudioPCI / CT4810",
    .priority          = 100,
    .probe             = ct4810_probe_entry,
    .get_name          = ct4810_get_name,
    .is_present        = ct4810_is_present,
    .set_format        = ct4810_set_format,
    .start_dma         = ct4810_start_dma,
    .stop_dma          = ct4810_stop_dma,
    .pause_dma         = ct4810_pause_dma,
    .set_volume        = ct4810_set_volume,
    .poll_half_needed  = ct4810_poll_half_needed,
    .write_half        = ct4810_write_half,
    .silence_half      = ct4810_silence_half,
    .get_dma_frame_pos = ct4810_get_dma_frame_pos,
    .get_dma_buffer    = ct4810_get_dma_buffer,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_audio_driver(&g_ct4810_drv);
}
