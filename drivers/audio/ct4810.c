/*
 * Ensoniq ES1371 / Creative CT4810 / CT5880 PCI audio driver (64-bit adaptation).
 *
 * Register definitions, sample-rate converter sequencing, and AC'97 access
 * follow the public Ensoniq ES1371 hardware documentation, Creative CT5880
 * documentation, and the Intel AC'97 Component Specification.
 */

#include "ct4810.h"
#include "dma.h"
#include "log.h"
#include "mm.h"
#include "pci.h"
#include "port.h"
#include "timer.h"

#define CT_SRC_STATE_MASK \
    (CT_SRC_DISABLE | CT_SRC_DAC1_DISABLE | CT_SRC_DAC2_DISABLE | CT_SRC_ADC_DISABLE)
#define CT_SRC_CODEC_ACCESS_STATE 0x00010000U
#define CT_SRC_CODEC_STATE_MASK   0x00870000U
#define CT_WAIT_LOOPS             0x8000

static int g_ct_present = 0;
static unsigned short g_ct_io_base = 0;
static const char* g_ct_chip_name = "None";
static char g_ct_display_name[72];
static unsigned char* g_ct_dma_buf = 0;
static unsigned int g_ct_dma_phys = 0;
static dma_buffer_t g_ct_dma;
static int g_ct_dma_running = 0;
static int g_ct_last_half_playing = -1;
static int g_ct_half_filled[2] = {0, 0};
static int g_ct_last_toggle_tick = 0;
static unsigned int g_ct_last_seen_frame = 0xFFFFFFFFU;
static int g_ct_hw_reg_moving = 0;
static unsigned char g_ct_revision = 0;
static int g_ct_is_ev1938 = 0;

static inline unsigned int ct_read32(unsigned int reg) {
    if (!g_ct_io_base) return 0;
    return inl((uint16_t)(g_ct_io_base + reg));
}

static inline void ct_write32(unsigned int reg, unsigned int val) {
    if (!g_ct_io_base) return;
    outl((uint16_t)(g_ct_io_base + reg), val);
}

static inline void ct_io_delay(void) {
    outb(0x80, 0);
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
    return 0;
}

static unsigned short ct_src_read_word(unsigned short reg) {
    unsigned int saved_state;
    unsigned int sampled_state;
    unsigned int control;

    if (!g_ct_io_base) return 0;

    saved_state = ct_src_wait_idle();
    control = (saved_state & CT_SRC_STATE_MASK) |
              CT_SRC_RAM_ADDRESS(reg) | CT_SRC_CODEC_ACCESS_STATE;
    ct_write32(CT_REG_SRC_CONTROL, control);

    sampled_state = ct_src_wait_idle();
    if ((sampled_state & CT_SRC_CODEC_STATE_MASK) !=
        CT_SRC_CODEC_ACCESS_STATE) {
        for (int t = 0; t < CT_WAIT_LOOPS; t++) {
            sampled_state = ct_read32(CT_REG_SRC_CONTROL);
            if ((sampled_state & CT_SRC_CODEC_STATE_MASK) ==
                CT_SRC_CODEC_ACCESS_STATE) break;
            ct_io_delay();
        }
    }

    control = (saved_state & CT_SRC_STATE_MASK) | CT_SRC_RAM_ADDRESS(reg);
    ct_write32(CT_REG_SRC_CONTROL, control);
    return (unsigned short)(sampled_state & 0xFFFFU);
}

static void ct_src_write_word(unsigned short reg, unsigned short data) {
    unsigned int r = ct_src_wait_idle() & CT_SRC_STATE_MASK;
    r |= CT_SRC_RAM_ADDRESS(reg) | (unsigned int)data;
    ct_write32(CT_REG_SRC_CONTROL, r | CT_SRC_RAM_WRITE_ENABLE);
}

static unsigned int ct_codec_access_begin(void) {
    unsigned int saved = ct_src_wait_idle();
    ct_write32(CT_REG_SRC_CONTROL,
               (saved & CT_SRC_STATE_MASK) | CT_SRC_CODEC_ACCESS_STATE);

    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        if ((ct_read32(CT_REG_SRC_CONTROL) & CT_SRC_CODEC_STATE_MASK) == 0) break;
        ct_io_delay();
    }
    for (int t = 0; t < CT_WAIT_LOOPS; t++) {
        if ((ct_read32(CT_REG_SRC_CONTROL) & CT_SRC_CODEC_STATE_MASK) ==
            CT_SRC_CODEC_ACCESS_STATE) break;
        ct_io_delay();
    }
    return saved;
}

static void ct_codec_access_end(unsigned int saved) {
    ct_src_wait_idle();
    ct_write32(CT_REG_SRC_CONTROL, saved);
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

    integer_field = ct_src_read_word((unsigned short)(base + CT_SRC_INTEGER_FREQUENCY));
    ct_src_write_word((unsigned short)(base + CT_SRC_INTEGER_FREQUENCY),
                      (unsigned short)((integer_field & 0x00FFU) |
                                      ((freq >> 5) & 0xFC00U)));
    ct_src_write_word((unsigned short)(base + CT_SRC_FRACTIONAL_FREQUENCY),
                      (unsigned short)(freq & 0x7FFFU));

    r = ct_src_wait_idle() & CT_SRC_STATE_MASK & ~disable;
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

int ct4810_init(void) {
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

    pci_scan_all();
    for (int index = 0; index < pci_get_device_count(); index++) {
        const pci_device_t* pci = pci_get_device(index);
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

        pci_enable_io(pci);
        pci_enable_memory(pci);
        pci_enable_bus_master(pci);

        unsigned int latency = pci_config_read(pci->bus, pci->slot,
                                               pci->function, 0x0C);
        if ((latency & 0xFF00U) < 0x4000U) {
            pci_config_write(pci->bus, pci->slot, pci->function,
                             0x0C, (latency & 0xFFFF00FFU) | 0x4000U);
        }

        g_ct_revision = pci->revision;
        g_ct_is_ev1938 = (device == CT_DEV_EV1938);

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

    if (!dma_is_valid(&g_ct_dma)) {
        g_ct_dma = dma_alloc_aligned(CT_DMA_BUFFER_BYTES, 65536);
        if (!dma_is_valid(&g_ct_dma)) {
            g_ct_present = 0;
            return -2;
        }
        g_ct_dma_buf = (unsigned char*)g_ct_dma.virt;
        g_ct_dma_phys = (unsigned int)g_ct_dma.phys;
    }

    for (int i = 0; i < CT_DMA_BUFFER_BYTES; i++) g_ct_dma_buf[i] = 0;
    g_ct_half_filled[0] = 0;
    g_ct_half_filled[1] = 0;

    ct_write32(CT_REG_CONTROL, 0);
    ct_write32(CT_REG_SERIAL_CONTROL, 0);
    ct_write32(CT_REG_LEGACY_CONTROL, 0);

    if (ct_needs_ac97_reset_quirk(vendor, device, g_ct_revision)) {
        status |= CT_STATUS_AC97_RESET;
    }
    if (status) {
        ct_write32(CT_REG_STATUS, status);
        sleep_ms(20);
    }

    ct_write32(CT_REG_CONTROL, CT_CONTROL_AC97_SYNC_RESET);
    (void)ct_read32(CT_REG_CONTROL);
    sleep_ms(2);
    ct_write32(CT_REG_CONTROL, 0);

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
    sleep_ms(50);
    unsigned short id1 = ct_ac97_read(CT_AC97_VENDOR_ID1);
    unsigned short id2 = ct_ac97_read(CT_AC97_VENDOR_ID2);
    if (id1 || id2) ct_set_codec_name(id1, id2);

    ct_ac97_write(CT_AC97_MASTER_VOLUME, 0x0000);
    ct_ac97_write(CT_AC97_HEADPHONE_VOLUME, 0x0000);
    ct_ac97_write(CT_AC97_PCM_OUT_VOLUME, 0x0000);
    ct_ac97_write(0x0A, 0x8000);
    ct_ac97_write(0x0E, 0x8008);
    ct_ac97_write(0x10, 0x8808);
    ct_ac97_write(0x12, 0x8808);

    ct4810_set_format(44100, 16, 1);

    ct_write32(CT_REG_MEMORY_PAGE, CT_PAGE_DAC);
    ct_write32(CT_REG_DAC2_FRAME, g_ct_dma_phys);
    ct_write32(CT_REG_DAC2_SIZE, (CT_DMA_BUFFER_BYTES / 4) - 1);
    ct_write32(CT_REG_DAC2_COUNT, (CT_DMA_HALF_BYTES / 4) - 1);

    klog_info("[CT48] active %s io=0x%x dma=0x%x",
              g_ct_chip_name, g_ct_io_base, g_ct_dma_phys);
    return 0;
}

int ct4810_is_present(void) {
    return g_ct_present;
}

const char* ct4810_get_name(void) {
    return g_ct_chip_name;
}

unsigned short ct4810_get_io_base(void) {
    return g_ct_io_base;
}

void ct4810_set_format(int rate, int bits, int stereo) {
    if (!g_ct_present || !g_ct_io_base) return;

    unsigned int mode = 0;
    if (bits == 16) mode |= 0x02U;
    if (stereo) mode |= 0x01U;

    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    sctrl &= ~(CT_SERIAL_P2_LOOPBACK | CT_SERIAL_P2_PAUSE | CT_SERIAL_P2_DAC_SLOT_ENABLE |
               CT_SERIAL_P2_END_MASK | CT_SERIAL_P2_START_MASK | CT_SERIAL_P2_MODE_MASK);
    sctrl |= CT_SERIAL_P2_INTERRUPT | CT_SERIAL_P2_MODE(mode) |
             CT_SERIAL_P2_END_INC(mode & 0x02U ? 2U : 1U) | CT_SERIAL_P2_START_INC(0);
    ct_write32(CT_REG_SERIAL_CONTROL, sctrl);

    ct_src_set_rate(CT_SRC_DAC2_BASE, rate);
}

void ct4810_start_dma(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_buf) return;

    ct_write32(CT_REG_MEMORY_PAGE, CT_PAGE_DAC);
    ct_write32(CT_REG_DAC2_FRAME, g_ct_dma_phys);
    ct_write32(CT_REG_DAC2_SIZE, (CT_DMA_BUFFER_BYTES / 4) - 1);
    ct_write32(CT_REG_DAC2_COUNT, (CT_DMA_HALF_BYTES / 4) - 1);

    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    sctrl &= ~(CT_SERIAL_P2_LOOPBACK | CT_SERIAL_P2_PAUSE);
    ct_write32(CT_REG_SERIAL_CONTROL, sctrl);

    unsigned int control = ct_read32(CT_REG_CONTROL);
    control &= ~CT_CONTROL_P2_BYPASS;
    control |= CT_CONTROL_BUS_MASTER_REQ | CT_CONTROL_DAC2_ENABLE;
    ct_write32(CT_REG_CONTROL, control);

    g_ct_dma_running = 1;
    g_ct_last_half_playing = 0;
    g_ct_last_toggle_tick = global_tick;
    g_ct_last_seen_frame = 0xFFFFFFFFU;
    g_ct_hw_reg_moving = 0;
}

void ct4810_stop_dma(void) {
    if (!g_ct_present || !g_ct_io_base) return;

    unsigned int control = ct_read32(CT_REG_CONTROL);
    control &= ~(CT_CONTROL_BUS_MASTER_REQ | CT_CONTROL_DAC2_ENABLE);
    ct_write32(CT_REG_CONTROL, control);

    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    ct_write32(CT_REG_SERIAL_CONTROL, sctrl | CT_SERIAL_P2_PAUSE);

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

void ct4810_pause_dma(int pause) {
    if (!g_ct_present || !g_ct_io_base) return;
    unsigned int sctrl = ct_read32(CT_REG_SERIAL_CONTROL);
    if (pause) sctrl |= CT_SERIAL_P2_PAUSE;
    else sctrl &= ~CT_SERIAL_P2_PAUSE;
    ct_write32(CT_REG_SERIAL_CONTROL, sctrl);
}

void ct4810_set_volume(int master_vol, int pcm_vol) {
    if (!g_ct_present || !g_ct_io_base) return;

    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    unsigned char m_att = (unsigned char)((100 - master_vol) * 31 / 100);
    unsigned char p_att = (unsigned char)((100 - pcm_vol) * 31 / 100);
    unsigned short m_val =
        (master_vol == 0) ? 0x8000U : (unsigned short)((m_att << 8) | m_att);
    unsigned short p_val =
        (pcm_vol == 0) ? 0x8000U : (unsigned short)((p_att << 8) | p_att);

    ct_ac97_write(CT_AC97_MASTER_VOLUME, m_val);
    ct_ac97_write(CT_AC97_HEADPHONE_VOLUME, m_val);
    ct_ac97_write(CT_AC97_PCM_OUT_VOLUME, p_val);
}

unsigned int ct4810_get_dma_frame_pos(void) {
    if (!g_ct_present || !g_ct_io_base || !g_ct_dma_running) return 0;

    ct_write32(CT_REG_MEMORY_PAGE, CT_PAGE_DAC);
    unsigned int val = ct_read32(CT_REG_DAC2_SIZE);
    unsigned int frame = (val >> 16) & 0xFFFFU;
    if (frame >= CT_DMA_TOTAL_FRAMES) frame = 0;
    return frame;
}

const void* ct4810_get_dma_buffer(void) {
    return g_ct_dma_buf;
}

void ct4810_write_half(int half_idx, const void* data, unsigned int bytes) {
    if (!g_ct_dma_buf) return;
    if (half_idx < 0 || half_idx > 1 || !data) return;
    if (bytes > CT_DMA_HALF_BYTES) bytes = CT_DMA_HALF_BYTES;

    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    const unsigned char* src = (const unsigned char*)data;
    for (unsigned int i = 0; i < bytes; i++) dest[i] = src[i];
    for (unsigned int i = bytes; i < CT_DMA_HALF_BYTES; i++) dest[i] = 0;

    dma_flush(&g_ct_dma);
    g_ct_half_filled[half_idx] = 1;
}

void ct4810_silence_half(int half_idx) {
    if (!g_ct_dma_buf || half_idx < 0 || half_idx > 1) return;
    unsigned char* dest = g_ct_dma_buf + half_idx * CT_DMA_HALF_BYTES;
    for (unsigned int i = 0; i < CT_DMA_HALF_BYTES; i++) dest[i] = 0;
    dma_flush(&g_ct_dma);
    g_ct_half_filled[half_idx] = 0;
}

int ct4810_poll_half_needed(void) {
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

    if (g_ct_last_toggle_tick == 0) g_ct_last_toggle_tick = global_tick;
    if ((global_tick - g_ct_last_toggle_tick) >= 19) {
        g_ct_last_toggle_tick = global_tick;
        int needed = g_ct_last_half_playing;
        g_ct_last_half_playing = 1 - g_ct_last_half_playing;
        g_ct_half_filled[needed] = 0;
        return needed;
    }

    if (!g_ct_half_filled[0]) return 0;
    if (!g_ct_half_filled[1]) return 1;
    return -1;
}

void ct4810_mark_half_filled(int half_idx) {
    if (half_idx >= 0 && half_idx < 2) g_ct_half_filled[half_idx] = 1;
}
