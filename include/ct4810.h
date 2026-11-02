#ifndef CT4810_H
#define CT4810_H

// Creative CT4810 / CT5880 / Ensoniq ES1371 / ES1373 PCI audio driver.

#define CT_VENDOR_ENSONIQ       0x1274
#define CT_DEV_ES1371           0x1371
#define CT_DEV_ES1371_COMPAT    0x0371
#define CT_DEV_CT5880           0x5880
#define CT_VENDOR_CREATIVE      0x1102
#define CT_DEV_EV1938           0x8938

// Controller direct I/O registers (BAR0 offsets).
#define CT_REG_CONTROL          0x00
#define CT_REG_STATUS           0x04
#define CT_REG_MEMORY_PAGE      0x0C
#define CT_REG_SRC_CONTROL      0x10
#define CT_REG_CODEC_PORT       0x14
#define CT_REG_LEGACY_CONTROL   0x18
#define CT_REG_SERIAL_CONTROL   0x20
#define CT_REG_DAC2_COUNT       0x28

// Memory-page selector and DAC page registers.
#define CT_PAGE_DAC             0x0C
#define CT_REG_DAC2_FRAME       0x38
#define CT_REG_DAC2_SIZE        0x3C

// Control register bits.
#define CT_CONTROL_P2_BYPASS         (1U << 30)
#define CT_CONTROL_AC97_SYNC_RESET   (1U << 14)
#define CT_CONTROL_BUS_MASTER_REQ    (1U << 7)
#define CT_CONTROL_DAC1_ENABLE       (1U << 6)
#define CT_CONTROL_DAC2_ENABLE       (1U << 5)
#define CT_CONTROL_ADC_ENABLE        (1U << 4)

// Status register bits.
#define CT_STATUS_AC97_RESET         (1U << 29)

// Serial channel 2 control fields.
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

// Sample-rate converter control fields and RAM locations.
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

// AC'97 codec port bits and standard registers.
#define CT_CODEC_READY               (1U << 31)
#define CT_CODEC_WRITE_BUSY          (1U << 30)
#define CT_CODEC_READ_REQUEST        (1U << 23)
#define CT_EV1938_CODEC_ACCESS       (1U << 26)
#define CT_AC97_RESET                0x00
#define CT_AC97_MASTER_VOLUME        0x02
#define CT_AC97_HEADPHONE_VOLUME     0x04
#define CT_AC97_PCM_OUT_VOLUME       0x18
#define CT_AC97_VENDOR_ID1           0x7C
#define CT_AC97_VENDOR_ID2           0x7E

// DMA Ring Buffer Dimensions: 64KB total, two 32KB halves
#define CT_DMA_BUFFER_BYTES     65536
#define CT_DMA_HALF_BYTES       32768
#define CT_DMA_TOTAL_FRAMES     16384 // in 16-bit stereo (4 bytes/frame)
#define CT_DMA_HALF_FRAMES      8192

int ct4810_init(void);
int ct4810_is_present(void);
const char* ct4810_get_name(void);
unsigned short ct4810_get_io_base(void);

void ct4810_set_format(int rate, int bits, int stereo);
void ct4810_start_dma(void);
void ct4810_stop_dma(void);
void ct4810_pause_dma(int pause);
void ct4810_set_volume(int master_vol, int pcm_vol);

unsigned int ct4810_get_dma_frame_pos(void);
const void* ct4810_get_dma_buffer(void);
void ct4810_write_half(int half_idx, const void* data, unsigned int bytes);
void ct4810_silence_half(int half_idx);
int ct4810_poll_half_needed(void);
void ct4810_mark_half_filled(int half_idx);

#endif // CT4810_H
