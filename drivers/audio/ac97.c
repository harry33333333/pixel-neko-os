/*
 * Intel ICH / AMD / NVIDIA AC'97 PCI Audio Controller Driver (64-bit kernel).
 *
 * Implements NAM (Native Audio Mixer) and NABM (Native Audio Bus Master)
 * ring-buffer DMA playback for Intel 82801 (ICH..ICH7), VirtualBox AC'97,
 * QEMU AC'97, and compatible controllers.
 */

#include "ac97.h"
#include "dma.h"
#include "log.h"
#include "pci.h"
#include "port.h"
#include "timer.h"

#define AC97_NAM_RESET            0x00
#define AC97_NAM_MASTER_VOL       0x02
#define AC97_NAM_HEADPHONE_VOL    0x04
#define AC97_NAM_PC_BEEP          0x0A
#define AC97_NAM_PCM_VOL          0x18
#define AC97_NAM_POWERDOWN        0x26
#define AC97_NAM_EXT_AUDIO_ID     0x28
#define AC97_NAM_EXT_AUDIO_CTRL   0x2A
#define AC97_NAM_FRONT_DAC_RATE   0x2C
#define AC97_NAM_VENDOR_ID1       0x7C
#define AC97_NAM_VENDOR_ID2       0x7E

#define AC97_NABM_PO_BDBAR        0x10
#define AC97_NABM_PO_CIV          0x14
#define AC97_NABM_PO_LVI          0x15
#define AC97_NABM_PO_SR           0x16
#define AC97_NABM_PO_PICB         0x18
#define AC97_NABM_PO_CR           0x1B
#define AC97_NABM_GLOB_CNT        0x2C
#define AC97_NABM_GLOB_STA        0x30

#define AC97_CR_RPBM              0x01U
#define AC97_CR_RR                0x02U

#define AC97_SR_DCH               0x0001U
#define AC97_SR_CELV              0x0002U
#define AC97_SR_LVBCI             0x0004U
#define AC97_SR_BCIS              0x0008U
#define AC97_SR_FIFOE             0x0010U
#define AC97_SR_W1C_MASK          0x001CU

#define AC97_BDL_ENTRIES          32
#define AC97_ENTRY_BYTES          (AC97_DMA_BUFFER_BYTES / AC97_BDL_ENTRIES) /* 2048 bytes */
#define AC97_ENTRY_SAMPLES        (AC97_ENTRY_BYTES / 2)                     /* 1024 16-bit samples */
#define AC97_ENTRY_FRAMES         (AC97_ENTRY_BYTES / 4)                     /* 512 stereo frames */

typedef struct {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

static int g_ac97_present = 0;
static uint16_t g_nam_base = 0;
static uint16_t g_nabm_base = 0;
static const char* g_ac97_name = "None";
static dma_buffer_t g_ac97_dma;
static dma_buffer_t g_ac97_bdl_dma;
static uint8_t* g_ac97_buf = 0;
static volatile ac97_bdl_entry_t* g_ac97_bdl = 0;
static int g_ac97_running = 0;
static int g_ac97_paused = 0;
static int g_ac97_last_half = -1;
static int g_ac97_half_filled[2] = {0, 0};

static inline uint16_t nam_read16(uint16_t reg) {
    return inw((uint16_t)(g_nam_base + reg));
}

static inline void nam_write16(uint16_t reg, uint16_t val) {
    outw((uint16_t)(g_nam_base + reg), val);
    outb(0x80, 0);
}

static inline uint8_t nabm_read8(uint16_t reg) {
    return inb((uint16_t)(g_nabm_base + reg));
}

static inline uint16_t nabm_read16(uint16_t reg) {
    return inw((uint16_t)(g_nabm_base + reg));
}

static inline uint32_t nabm_read32(uint16_t reg) {
    return inl((uint16_t)(g_nabm_base + reg));
}

static inline void nabm_write8(uint16_t reg, uint8_t val) {
    outb((uint16_t)(g_nabm_base + reg), val);
}

static inline void nabm_write16(uint16_t reg, uint16_t val) {
    outw((uint16_t)(g_nabm_base + reg), val);
}

static inline void nabm_write32(uint16_t reg, uint32_t val) {
    outl((uint16_t)(g_nabm_base + reg), val);
}

static int ac97_match_controller(const pci_device_t* pci, const char** out_name) {
    if (!pci) return 0;
    if (pci->class_code != 0x04 || pci->subclass != 0x01) return 0;

    if (pci->vendor == 0x8086) {
        switch (pci->device) {
            case 0x2415: *out_name = "Intel 82801AA (ICH) AC'97 Audio"; return 1;
            case 0x2425: *out_name = "Intel 82801AB (ICH0) AC'97 Audio"; return 1;
            case 0x2445: *out_name = "Intel 82801BA (ICH2) AC'97 Audio"; return 1;
            case 0x2485: *out_name = "Intel 82801CA (ICH3) AC'97 Audio"; return 1;
            case 0x24C5: *out_name = "Intel 82801DB (ICH4) AC'97 Audio"; return 1;
            case 0x24D5: *out_name = "Intel 82801EB (ICH5) AC'97 Audio"; return 1;
            case 0x266E: *out_name = "Intel 82801FB (ICH6) AC'97 Audio"; return 1;
            case 0x27DE: *out_name = "Intel 82801GB (ICH7) AC'97 Audio"; return 1;
            default:     *out_name = "Intel ICH AC'97 Audio"; return 1;
        }
    }
    if (pci->vendor == 0x1022 && (pci->device == 0x7445 || pci->device == 0x746D)) {
        *out_name = "AMD AC'97 Audio Controller";
        return 1;
    }
    if (pci->vendor == 0x10DE) {
        *out_name = "NVIDIA nForce AC'97 Audio";
        return 1;
    }
    if (pci->vendor == 0x1039 && pci->device == 0x7012) {
        *out_name = "SiS 7012 AC'97 Audio";
        return 1;
    }
    return 0;
}

static void ac97_setup_bdl(void) {
    if (!g_ac97_bdl || !dma_is_valid(&g_ac97_dma)) return;
    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        g_ac97_bdl[i].addr = (uint32_t)g_ac97_dma.phys + (uint32_t)(i * AC97_ENTRY_BYTES);
        g_ac97_bdl[i].samples = AC97_ENTRY_SAMPLES;
        g_ac97_bdl[i].flags = 0;
    }
    dma_flush(&g_ac97_bdl_dma);
}

int ac97_init(void) {
    g_ac97_present = 0;
    g_nam_base = 0;
    g_nabm_base = 0;
    g_ac97_running = 0;
    g_ac97_paused = 0;

    pci_scan_all();
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* pci = pci_get_device(i);
        const char* name = 0;
        if (!ac97_match_controller(pci, &name)) continue;

        if (!pci->bar[0].valid || !pci->bar[0].is_io ||
            !pci->bar[1].valid || !pci->bar[1].is_io) {
            continue;
        }

        g_nam_base = (uint16_t)pci->bar[0].address;
        g_nabm_base = (uint16_t)pci->bar[1].address;
        g_ac97_name = name;

        pci_enable_io(pci);
        pci_enable_memory(pci);
        pci_enable_bus_master(pci);
        g_ac97_present = 1;
        break;
    }

    if (!g_ac97_present || !g_nam_base || !g_nabm_base) {
        return -1;
    }

    if (!dma_is_valid(&g_ac97_dma)) {
        g_ac97_dma = dma_alloc_aligned(AC97_DMA_BUFFER_BYTES, 65536);
        if (!dma_is_valid(&g_ac97_dma)) {
            g_ac97_present = 0;
            return -2;
        }
        g_ac97_buf = (uint8_t*)g_ac97_dma.virt;
    }
    if (!dma_is_valid(&g_ac97_bdl_dma)) {
        g_ac97_bdl_dma = dma_alloc_aligned(sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES, 4096);
        if (!dma_is_valid(&g_ac97_bdl_dma)) {
            g_ac97_present = 0;
            return -2;
        }
        g_ac97_bdl = (volatile ac97_bdl_entry_t*)g_ac97_bdl_dma.virt;
    }

    for (int i = 0; i < AC97_DMA_BUFFER_BYTES; i++) g_ac97_buf[i] = 0;
    ac97_setup_bdl();

    /* Take AC-link out of cold reset */
    uint32_t glob_cnt = nabm_read32(AC97_NABM_GLOB_CNT);
    glob_cnt &= ~0x00000030U; /* 2-channel mode, 16-bit */
    glob_cnt |= 0x00000002U;  /* Deassert cold reset */
    nabm_write32(AC97_NABM_GLOB_CNT, glob_cnt);
    sleep_ms(10);

    /* Reset NAM mixer */
    nam_write16(AC97_NAM_RESET, 0x0000);
    sleep_ms(20);
    nam_write16(AC97_NAM_POWERDOWN, 0x0000);
    sleep_ms(10);

    /* Enable Variable Rate Audio (VRA) if supported */
    uint16_t ext_id = nam_read16(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001U) {
        uint16_t ext_ctrl = nam_read16(AC97_NAM_EXT_AUDIO_CTRL);
        nam_write16(AC97_NAM_EXT_AUDIO_CTRL, (uint16_t)(ext_ctrl | 0x0001U));
    }

    nam_write16(AC97_NAM_PC_BEEP, 0x8000U);
    ac97_set_volume(85, 85);
    ac97_set_format(48000, 16, 1);

    klog_info("[AC97] active %s NAM=0x%x NABM=0x%x",
              g_ac97_name, g_nam_base, g_nabm_base);
    return 0;
}

int ac97_is_present(void) {
    return g_ac97_present;
}

const char* ac97_get_name(void) {
    return g_ac97_name;
}

void ac97_set_format(int rate, int bits, int stereo) {
    (void)bits;
    (void)stereo;
    if (!g_ac97_present) return;
    if (rate < 8000) rate = 8000;
    if (rate > 48000) rate = 48000;

    uint16_t ext_id = nam_read16(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001U) {
        uint16_t ext_ctrl = nam_read16(AC97_NAM_EXT_AUDIO_CTRL);
        nam_write16(AC97_NAM_EXT_AUDIO_CTRL, (uint16_t)(ext_ctrl | 0x0001U));
        nam_write16(AC97_NAM_FRONT_DAC_RATE, (uint16_t)rate);
    }
}

void ac97_start_dma(void) {
    if (!g_ac97_present || !g_ac97_bdl) return;

    ac97_setup_bdl();

    /* Reset PCM Out DMA engine */
    nabm_write8(AC97_NABM_PO_CR, 0);
    nabm_write8(AC97_NABM_PO_CR, AC97_CR_RR);
    for (int i = 0; i < 10000; i++) {
        if (!(nabm_read8(AC97_NABM_PO_CR) & AC97_CR_RR)) break;
        outb(0x80, 0);
    }
    nabm_write16(AC97_NABM_PO_SR, AC97_SR_W1C_MASK);

    nabm_write32(AC97_NABM_PO_BDBAR, (uint32_t)g_ac97_bdl_dma.phys);
    nabm_write8(AC97_NABM_PO_LVI, AC97_BDL_ENTRIES - 1);
    nabm_write8(AC97_NABM_PO_CR, AC97_CR_RPBM);

    g_ac97_running = 1;
    g_ac97_paused = 0;
    g_ac97_last_half = 0;
}

void ac97_stop_dma(void) {
    if (!g_ac97_present) return;
    nabm_write8(AC97_NABM_PO_CR, 0);
    nabm_write8(AC97_NABM_PO_CR, AC97_CR_RR);
    nabm_write16(AC97_NABM_PO_SR, AC97_SR_W1C_MASK);
    g_ac97_running = 0;
    g_ac97_paused = 0;
    g_ac97_last_half = -1;
    g_ac97_half_filled[0] = 0;
    g_ac97_half_filled[1] = 0;
    if (g_ac97_buf) {
        for (int i = 0; i < AC97_DMA_BUFFER_BYTES; i++) g_ac97_buf[i] = 0;
    }
}

void ac97_pause_dma(int pause) {
    if (!g_ac97_present || !g_ac97_running) return;
    if (pause) {
        nabm_write8(AC97_NABM_PO_CR, 0);
        g_ac97_paused = 1;
    } else {
        nabm_write8(AC97_NABM_PO_CR, AC97_CR_RPBM);
        g_ac97_paused = 0;
    }
}

void ac97_set_volume(int master_vol, int pcm_vol) {
    if (!g_ac97_present) return;
    if (master_vol < 0) master_vol = 0;
    if (master_vol > 100) master_vol = 100;
    if (pcm_vol < 0) pcm_vol = 0;
    if (pcm_vol > 100) pcm_vol = 100;

    uint16_t m_att = (uint16_t)(((100 - master_vol) * 31) / 100);
    uint16_t p_att = (uint16_t)(((100 - pcm_vol) * 31) / 100);

    uint16_t m_val = (master_vol == 0) ? 0x8000U : (uint16_t)((m_att << 8) | m_att);
    uint16_t p_val = (pcm_vol == 0) ? 0x8000U : (uint16_t)((p_att << 8) | p_att);

    nam_write16(AC97_NAM_MASTER_VOL, m_val);
    nam_write16(AC97_NAM_HEADPHONE_VOL, m_val);
    nam_write16(AC97_NAM_PCM_VOL, p_val);
}

unsigned int ac97_get_dma_frame_pos(void) {
    if (!g_ac97_present || !g_ac97_running) return 0;
    uint8_t civ = nabm_read8(AC97_NABM_PO_CIV) & 0x1FU;
    uint16_t picb = nabm_read16(AC97_NABM_PO_PICB);
    if (picb > AC97_ENTRY_SAMPLES) picb = AC97_ENTRY_SAMPLES;
    unsigned int frames_done_in_entry = (AC97_ENTRY_SAMPLES - picb) / 2U;
    unsigned int pos = (unsigned int)civ * AC97_ENTRY_FRAMES + frames_done_in_entry;
    if (pos >= AC97_DMA_TOTAL_FRAMES) pos = 0;
    return pos;
}

const void* ac97_get_dma_buffer(void) {
    return g_ac97_buf;
}

void ac97_write_half(int half_idx, const void* data, unsigned int bytes) {
    if (!g_ac97_buf || (half_idx != 0 && half_idx != 1) || !data) return;
    if (bytes > AC97_DMA_HALF_BYTES) bytes = AC97_DMA_HALF_BYTES;

    uint8_t* dst = g_ac97_buf + (half_idx * AC97_DMA_HALF_BYTES);
    const uint8_t* src = (const uint8_t*)data;
    for (unsigned int i = 0; i < bytes; i++) dst[i] = src[i];
    for (unsigned int i = bytes; i < AC97_DMA_HALF_BYTES; i++) dst[i] = 0;
    dma_flush(&g_ac97_dma);
    g_ac97_half_filled[half_idx] = 1;
}

void ac97_silence_half(int half_idx) {
    if (!g_ac97_buf || (half_idx != 0 && half_idx != 1)) return;
    uint8_t* dst = g_ac97_buf + (half_idx * AC97_DMA_HALF_BYTES);
    for (unsigned int i = 0; i < AC97_DMA_HALF_BYTES; i++) dst[i] = 0;
    dma_flush(&g_ac97_dma);
    g_ac97_half_filled[half_idx] = 0;
}

int ac97_poll_half_needed(void) {
    if (!g_ac97_present || !g_ac97_running || g_ac97_paused) return -1;

    uint8_t civ = nabm_read8(AC97_NABM_PO_CIV) & 0x1FU;
    uint8_t target_lvi = (uint8_t)((civ + AC97_BDL_ENTRIES - 1) & 0x1FU);
    nabm_write8(AC97_NABM_PO_LVI, target_lvi);

    uint16_t sr = nabm_read16(AC97_NABM_PO_SR);
    if (sr & AC97_SR_W1C_MASK) {
        nabm_write16(AC97_NABM_PO_SR, (uint16_t)(sr & AC97_SR_W1C_MASK));
    }
    if (sr & AC97_SR_DCH) {
        nabm_write8(AC97_NABM_PO_CR, AC97_CR_RPBM);
    }

    int current_half = (civ < 16) ? 0 : 1;
    if (g_ac97_last_half < 0) {
        g_ac97_last_half = current_half;
        return -1;
    }
    if (current_half != g_ac97_last_half) {
        int finished = g_ac97_last_half;
        g_ac97_last_half = current_half;
        g_ac97_half_filled[finished] = 0;
        return finished;
    }
    return -1;
}
