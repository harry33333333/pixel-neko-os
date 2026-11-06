#include "fdc.h"
#include "dma.h"
#include "isadma.h"
#include "log.h"
#include "port.h"
#include "string.h"
#include "usb_core.h"

#define FDC_BASE                 0x3F0
#define FDC_DOR                  (FDC_BASE + 2)
#define FDC_MSR                  (FDC_BASE + 4)
#define FDC_FIFO                 (FDC_BASE + 5)
#define FDC_DIR                  (FDC_BASE + 7)
#define FDC_CCR                  (FDC_BASE + 7)

#define FDC_MSR_RQM              0x80
#define FDC_MSR_DIO              0x40
#define FDC_MSR_NDMA             0x20
#define FDC_MSR_CB               0x10

#define FDC_DOR_RESET            0x04
#define FDC_DOR_DMA              0x08

#define FDC_CMD_SPECIFY          0x03
#define FDC_CMD_RECALIBRATE      0x07
#define FDC_CMD_SENSE_INTERRUPT  0x08
#define FDC_CMD_SEEK             0x0F
#define FDC_CMD_WRITE_DATA       0xC5
#define FDC_CMD_READ_DATA        0xE6

#define FDC_RATE_500K            0x00
#define FDC_RATE_300K            0x01
#define FDC_RATE_250K            0x02
#define FDC_RATE_1M              0x03

#define FDC_SECTOR_SIZE          512u
#define FDC_MAX_BATCH_SECTORS    32u
#define FDC_DMA_SIZE             (FDC_MAX_BATCH_SECTORS * FDC_SECTOR_SIZE)
#define FDC_MAX_TOTAL_SECTORS    10000u
#define FDC_MOTOR_IDLE_POLLS     3

typedef struct {
    bool hardware_present;
    bool media_valid;
    bool ejected;
    bool motor_on;
    int motor_idle_polls;
    uint8_t data_rate;
    uint8_t cylinders;
    uint8_t heads;
    uint8_t sectors_per_track;
    uint32_t total_sectors;
    int current_cylinder;
    uint8_t last_change_bit;
    int absent_probe_delay;
    blkdev_t* block;
} fdc_drive_t;

typedef struct {
    uint8_t rate;
    uint8_t heads;
    uint8_t sectors_per_track;
    uint8_t cylinders;
    uint16_t total_sectors;
} fdc_geometry_t;

static fdc_drive_t g_fdc_drives[2];
static dma_buffer_t g_fdc_dma;
static bool g_fdc_dma_ready;
static bool g_fdc_controller_ready;
static bool g_fdc_enhanced;

static const fdc_geometry_t g_geometry_probes[] = {
    { FDC_RATE_500K, 2, 18, 80, 2880 },
    { FDC_RATE_500K, 2, 15, 80, 2400 },
    { FDC_RATE_250K, 2, 9, 80, 1440 },
    { FDC_RATE_300K, 2, 9, 40, 720 },
    { FDC_RATE_250K, 2, 8, 40, 640 },
    { FDC_RATE_300K, 1, 9, 40, 360 },
    { FDC_RATE_250K, 1, 8, 40, 320 },
    { FDC_RATE_1M, 2, 36, 80, 5760 }
};

static uint16_t read_le16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void fdc_io_delay(void) {
    for (int index = 0; index < 4; index++) (void)inb(0x80);
}

static uint8_t fdc_cmos_read(uint8_t index) {
    outb(0x70, index);
    fdc_io_delay();
    return inb(0x71);
}

static int fdc_wait_fifo(bool data_direction, uint32_t timeout_ms) {
    for (uint32_t elapsed = 0; elapsed <= timeout_ms; elapsed++) {
        uint8_t status = inb(FDC_MSR);
        if (status == 0xFF) return -1;
        bool dio = (status & FDC_MSR_DIO) != 0;
        if ((status & FDC_MSR_RQM) && dio == data_direction) return 0;
        usb_delay_ms(1);
    }
    return -1;
}

static int fdc_write_byte(uint8_t value) {
    if (fdc_wait_fifo(false, 100) != 0) return -1;
    outb(FDC_FIFO, value);
    fdc_io_delay();
    return 0;
}

static int fdc_read_byte(uint8_t* value, uint32_t timeout_ms) {
    if (!value || fdc_wait_fifo(true, timeout_ms) != 0) return -1;
    *value = inb(FDC_FIFO);
    fdc_io_delay();
    return 0;
}

static void fdc_select_drive(int drive, bool motor) {
    uint8_t dor = FDC_DOR_DMA | FDC_DOR_RESET | (uint8_t)(drive & 1);
    if (motor) dor |= (uint8_t)(0x10u << (drive & 1));
    outb(FDC_DOR, dor);
    fdc_io_delay();
}

static void fdc_motor_off(fdc_drive_t* drive) {
    if (!drive) return;
    drive->motor_on = false;
    drive->motor_idle_polls = 0;
    fdc_select_drive((int)(drive - g_fdc_drives), false);
}

static int fdc_motor_on(fdc_drive_t* drive) {
    if (!drive) return -1;
    int index = (int)(drive - g_fdc_drives);
    fdc_select_drive(index, true);
    if (!drive->motor_on) {
        usb_delay_ms(500);
        drive->motor_on = true;
    }
    drive->motor_idle_polls = 0;
    return 0;
}

static void fdc_set_data_rate(uint8_t rate) {
    outb(FDC_CCR, (uint8_t)(rate & 0x03u));
    fdc_io_delay();
}

static int fdc_specify(void) {
    if (fdc_write_byte(FDC_CMD_SPECIFY) != 0 ||
        fdc_write_byte(0x3F) != 0 ||
        fdc_write_byte(0x40) != 0) {
        return -1;
    }
    return 0;
}

static int fdc_sense_interrupt_raw(uint8_t* status, uint8_t* cylinder) {
    if (!status || !cylinder) return -1;
    if (fdc_write_byte(FDC_CMD_SENSE_INTERRUPT) != 0 ||
        fdc_read_byte(status, 150) != 0 ||
        fdc_read_byte(cylinder, 150) != 0) {
        return -1;
    }
    return 0;
}

static int fdc_controller_reset(void) {
    isadma_disable();
    outb(FDC_DOR, 0);
    usb_delay_ms(20);
    outb(FDC_DOR, FDC_DOR_DMA | FDC_DOR_RESET);
    fdc_io_delay();
    usb_delay_ms(100);

    if (inb(FDC_MSR) == 0xFF) {
        log_printf("[FDC ] controller not present\n");
        return -1;
    }

    int reset_responses = 0;
    for (int index = 0; index < 4; index++) {
        uint8_t status = 0;
        uint8_t cylinder = 0;
        if (fdc_sense_interrupt_raw(&status, &cylinder) != 0) break;
        reset_responses++;
    }

    fdc_set_data_rate(FDC_RATE_250K);
    if (fdc_specify() != 0) {
        log_printf("[FDC ] SPECIFY failed\n");
        return -1;
    }
    log_printf("[FDC ] controller reset complete, interrupt responses=%d\n",
               reset_responses);
    return 0;
}

static int fdc_recalibrate(int drive_index) {
    fdc_select_drive(drive_index, true);
    if (fdc_write_byte(FDC_CMD_RECALIBRATE) != 0 ||
        fdc_write_byte((uint8_t)(drive_index & 1)) != 0) {
        return -1;
    }
    usb_delay_ms(300);

    uint8_t status = 0;
    uint8_t cylinder = 0;
    if (fdc_sense_interrupt_raw(&status, &cylinder) != 0) return -1;
    if ((status & 0xE0u) != 0x20u ||
        (status & 0x03u) != (uint8_t)(drive_index & 1) ||
        cylinder != 0) {
        return -1;
    }
    g_fdc_drives[drive_index].current_cylinder = 0;
    return 0;
}

static int fdc_seek(int drive_index, uint8_t cylinder) {
    fdc_drive_t* drive = &g_fdc_drives[drive_index];
    if (drive->current_cylinder == cylinder) return 0;

    fdc_select_drive(drive_index, true);
    if (fdc_write_byte(FDC_CMD_SEEK) != 0 ||
        fdc_write_byte((uint8_t)(drive_index & 1)) != 0 ||
        fdc_write_byte(cylinder) != 0) {
        return -1;
    }
    int distance = (int)cylinder - drive->current_cylinder;
    if (drive->current_cylinder < 0) distance = 80;
    if (distance < 0) distance = -distance;
    uint32_t settle_ms = 20u + (uint32_t)distance * 3u;
    if (settle_ms > 300u) settle_ms = 300u;
    usb_delay_ms(settle_ms);

    uint8_t status = 0;
    uint8_t current = 0;
    if (fdc_sense_interrupt_raw(&status, &current) != 0) return -1;
    if ((status & 0xE0u) != 0x20u ||
        (status & 0x03u) != (uint8_t)(drive_index & 1) ||
        current != cylinder) {
        drive->current_cylinder = -1;
        return -1;
    }
    drive->current_cylinder = cylinder;
    return 0;
}

static int fdc_read_result(uint8_t result[7], uint32_t timeout_ms) {
    for (int index = 0; index < 7; index++) {
        if (fdc_read_byte(&result[index], timeout_ms) != 0) return -1;
    }
    return 0;
}

static int fdc_transfer_chunk(int drive_index, bool write,
                              uint32_t lba, uint32_t count,
                              const uint8_t* input, uint8_t* output) {
    fdc_drive_t* drive = &g_fdc_drives[drive_index];
    if (!drive->hardware_present || !drive->media_valid ||
        count == 0 || count > FDC_MAX_BATCH_SECTORS ||
        drive->heads == 0 || drive->sectors_per_track == 0 ||
        lba >= drive->total_sectors ||
        count > drive->total_sectors - lba) {
        return -1;
    }

    uint32_t sectors_per_cylinder =
        (uint32_t)drive->heads * drive->sectors_per_track;
    uint32_t cylinder = lba / sectors_per_cylinder;
    uint32_t remainder = lba % sectors_per_cylinder;
    uint32_t head = remainder / drive->sectors_per_track;
    uint32_t sector = remainder % drive->sectors_per_track + 1u;
    if (cylinder >= drive->cylinders) return -1;
    if (sector + count - 1u > drive->sectors_per_track) return -1;

    if (fdc_motor_on(drive) != 0) return -1;
    fdc_set_data_rate(drive->data_rate);
    if (fdc_seek(drive_index, (uint8_t)cylinder) != 0) return -1;

    unsigned int bytes = count * FDC_SECTOR_SIZE;
    uint8_t* dma_buffer = (uint8_t*)g_fdc_dma.virt_addr;
    if (!dma_buffer) return -1;
    if (write) {
        if (!input) return -1;
        memcpy(dma_buffer, input, bytes);
        if (!isadma_setup_write(dma_buffer, bytes)) return -1;
    } else {
        if (!output) return -1;
        if (!isadma_setup_read(dma_buffer, bytes)) return -1;
    }

    uint8_t gap = (drive->data_rate == FDC_RATE_250K ||
                   drive->data_rate == FDC_RATE_300K) ? 0x2Au : 0x1Bu;
    uint8_t command = write ? FDC_CMD_WRITE_DATA : FDC_CMD_READ_DATA;
    if (fdc_write_byte(command) != 0 ||
        fdc_write_byte((uint8_t)((head << 2) | (uint8_t)drive_index)) != 0 ||
        fdc_write_byte((uint8_t)cylinder) != 0 ||
        fdc_write_byte((uint8_t)head) != 0 ||
        fdc_write_byte((uint8_t)sector) != 0 ||
        fdc_write_byte(2) != 0 ||
        fdc_write_byte(drive->sectors_per_track) != 0 ||
        fdc_write_byte(gap) != 0 ||
        fdc_write_byte(0xFF) != 0) {
        isadma_disable();
        return -1;
    }

    uint8_t result[7];
    int result_status = fdc_read_result(result, write ? 5000 : 4000);
    isadma_disable();
    if (result_status != 0) return -1;
    if ((result[0] & 0xC0u) != 0 || result[1] != 0 || result[2] != 0) {
        return -1;
    }
    if (!write) {
        __sync_synchronize();
        if (output != dma_buffer) memcpy(output, dma_buffer, bytes);
    }
    drive->current_cylinder = (int)cylinder;
    drive->motor_idle_polls = 0;
    return 0;
}

static bool fdc_decode_bpb(fdc_drive_t* drive, const uint8_t* sector,
                           uint8_t rate) {
    if (!drive || !sector) return false;

    uint16_t bytes_per_sector = read_le16(sector + 11);
    uint8_t sectors_per_cluster = sector[13];
    uint16_t reserved = read_le16(sector + 14);
    uint8_t fat_count = sector[16];
    uint16_t root_entries = read_le16(sector + 17);
    uint16_t total16 = read_le16(sector + 19);
    uint8_t media_descriptor = sector[21];
    uint16_t fat16 = read_le16(sector + 22);
    uint16_t sectors_per_track = read_le16(sector + 24);
    uint16_t heads = read_le16(sector + 26);
    uint32_t total32 = read_le32(sector + 32);
    uint32_t fat32 = read_le32(sector + 36);
    uint32_t total = total16 ? total16 : total32;
    uint32_t fat_size = fat16 ? fat16 : fat32;

    if (bytes_per_sector != FDC_SECTOR_SIZE ||
        (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0 ||
        sectors_per_cluster == 0 || reserved == 0 ||
        fat_count == 0 || fat_count > 4 ||
        sectors_per_track < 8 || sectors_per_track > 40 ||
        heads < 1 || heads > 2 ||
        total == 0 || total > FDC_MAX_TOTAL_SECTORS ||
        media_descriptor < 0xF0 || fat_size == 0) {
        return false;
    }

    uint32_t root_sectors =
        ((uint32_t)root_entries * 32u + FDC_SECTOR_SIZE - 1u) /
        FDC_SECTOR_SIZE;
    uint32_t metadata =
        reserved + (uint32_t)fat_count * fat_size + root_sectors;
    if (total <= metadata) return false;

    uint32_t sectors_per_cylinder =
        (uint32_t)heads * sectors_per_track;
    uint32_t cylinders = total / sectors_per_cylinder;
    if (cylinders < 35 || cylinders > 85 ||
        total % sectors_per_cylinder != 0) {
        return false;
    }

    drive->data_rate = rate;
    drive->cylinders = (uint8_t)cylinders;
    drive->heads = (uint8_t)heads;
    drive->sectors_per_track = (uint8_t)sectors_per_track;
    drive->total_sectors = total;
    drive->current_cylinder = 0;
    return true;
}

static media_state_t fdc_detect_media_locked(int drive_index) {
    fdc_drive_t* drive = &g_fdc_drives[drive_index];
    if (!drive->hardware_present) return MEDIA_ABSENT;
    if (fdc_motor_on(drive) != 0) return MEDIA_ERROR;

    if (fdc_recalibrate(drive_index) != 0) {
        drive->media_valid = false;
        drive->total_sectors = 0;
        return MEDIA_ABSENT;
    }

    for (unsigned int index = 0;
         index < sizeof(g_geometry_probes) / sizeof(g_geometry_probes[0]);
         index++) {
        const fdc_geometry_t* probe = &g_geometry_probes[index];
        if (probe->rate == FDC_RATE_1M && !g_fdc_enhanced) continue;

        drive->data_rate = probe->rate;
        drive->cylinders = probe->cylinders;
        drive->heads = probe->heads;
        drive->sectors_per_track = probe->sectors_per_track;
        drive->total_sectors = probe->total_sectors;
        drive->current_cylinder = 0;
        drive->media_valid = true;

        if (fdc_transfer_chunk(drive_index, false, 0, 1, NULL,
                               (uint8_t*)g_fdc_dma.virt_addr) == 0 &&
            fdc_decode_bpb(drive, (const uint8_t*)g_fdc_dma.virt_addr,
                           probe->rate)) {
            drive->media_valid = true;
            drive->ejected = false;
            drive->last_change_bit =
                (uint8_t)((inb(FDC_DIR) & 0x80u) ? 1u : 0u);
            drive->absent_probe_delay = 0;
            return MEDIA_PRESENT;
        }
    }

    drive->media_valid = false;
    drive->total_sectors = 0;
    drive->current_cylinder = -1;
    drive->absent_probe_delay = 2;
    return MEDIA_ABSENT;
}

static int fdc_probe_hardware(int drive_index) {
    if (fdc_motor_on(&g_fdc_drives[drive_index]) != 0) return 0;
    if (fdc_recalibrate(drive_index) != 0) {
        fdc_motor_off(&g_fdc_drives[drive_index]);
        return 0;
    }
    return 1;
}

static void fdc_update_block_capacity(fdc_drive_t* drive,
                                      media_state_t state,
                                      bool force_generation) {
    if (!drive || !drive->block) return;
    if (state == MEDIA_PRESENT) {
        if (force_generation ||
            drive->block->total_sectors != drive->total_sectors ||
            drive->block->sector_size != FDC_SECTOR_SIZE) {
            block_set_capacity(drive->block, drive->total_sectors,
                               FDC_SECTOR_SIZE);
        } else {
            drive->block->media_state = MEDIA_PRESENT;
        }
    } else if (drive->block->total_sectors != 0) {
        block_set_capacity(drive->block, 0, FDC_SECTOR_SIZE);
    }
}

static int fdc_block_read(blkdev_t* block, uint64_t lba, uint32_t count,
                          void* buffer) {
    if (!block || lba > UINT32_MAX) return -1;
    return fdc_read_sectors(block->unit, (unsigned int)lba, count,
                            (unsigned char*)buffer);
}

static int fdc_block_write(blkdev_t* block, uint64_t lba, uint32_t count,
                           const void* buffer) {
    if (!block || lba > UINT32_MAX) return -1;
    return fdc_write_sectors(block->unit, (unsigned int)lba, count,
                             (const unsigned char*)buffer);
}

static media_state_t fdc_block_check_media(blkdev_t* block) {
    return block ? fdc_check_media(block->unit) : MEDIA_ERROR;
}

int fdc_init(void) {
    memset(g_fdc_drives, 0, sizeof(g_fdc_drives));
    memset(&g_fdc_dma, 0, sizeof(g_fdc_dma));
    g_fdc_dma_ready = false;
    g_fdc_controller_ready = false;
    g_fdc_enhanced = false;

    if (dma_alloc(FDC_DMA_SIZE, DMA_BIT_MASK(24), PAGE_SIZE,
                  &g_fdc_dma) != 0) {
        log_printf("[FDC ] unable to allocate ISA DMA buffer\n");
        return -1;
    }
    g_fdc_dma_ready = true;

    if (fdc_controller_reset() != 0) {
        isadma_disable();
        return -1;
    }
    g_fdc_controller_ready = true;

    uint8_t cmos_types = fdc_cmos_read(0x10);
    uint8_t equipment = fdc_cmos_read(0x14);
    g_fdc_enhanced =
        ((cmos_types >> 4) & 0x0Fu) == 0x05u ||
        (cmos_types & 0x0Fu) == 0x05u;
    bool hints[2] = {
        ((cmos_types >> 4) & 0x0Fu) != 0,
        (cmos_types & 0x0Fu) != 0
    };
    if (equipment & 0x01u) {
        int count = (int)((equipment >> 6) & 0x03u) + 1;
        if (count > 2) count = 2;
        for (int index = 0; index < count; index++) hints[index] = true;
    }
    if (!hints[0] && !hints[1]) {
        log_printf("[FDC ] no drives reported by firmware\n");
        isadma_disable();
        return 0;
    }

    int detected = 0;
    for (int index = 0; index < 2; index++) {
        fdc_drive_t* drive = &g_fdc_drives[index];
        if (!hints[index] || !fdc_probe_hardware(index)) continue;

        drive->hardware_present = true;
        (void)fdc_detect_media_locked(index);
        char name[16] = { 'f', 'd', 'c', (char)('0' + index), '\0' };
        drive->block = block_register(
            name, BLK_TYPE_FLOPPY, index, FDC_SECTOR_SIZE,
            drive->total_sectors, false, true,
            fdc_block_read, fdc_block_write, fdc_block_check_media,
            drive);
        if (drive->block) {
            memcpy(drive->block->model, "ISA Floppy Controller",
                   sizeof("ISA Floppy Controller"));
            if (drive->media_valid) {
                log_printf(
                    "[FDC ] %c: %uKiB %u cyl / %u head / %u spt\n",
                    'A' + index, drive->total_sectors / 2,
                    drive->cylinders, drive->heads,
                    drive->sectors_per_track);
            } else {
                log_printf("[FDC ] %c: drive present, no media\n",
                           'A' + index);
            }
            detected++;
        }
        fdc_motor_off(drive);
    }

    isadma_disable();
    log_printf("[FDC ] controller=%s drives=%d\n",
               g_fdc_enhanced ? "enhanced" : "compatible",
               detected);
    return detected;
}

int fdc_has_drive(int drive) {
    return drive >= 0 && drive < 2 && g_fdc_drives[drive].hardware_present;
}

unsigned int fdc_get_total_sectors(int drive) {
    return drive >= 0 && drive < 2 ? g_fdc_drives[drive].total_sectors : 0;
}

media_state_t fdc_check_media(int drive) {
    if (drive < 0 || drive >= 2 || !g_fdc_controller_ready ||
        !g_fdc_dma_ready || !g_fdc_drives[drive].hardware_present) {
        return MEDIA_ABSENT;
    }

    fdc_drive_t* state = &g_fdc_drives[drive];
    uint8_t change_bit = (uint8_t)((inb(FDC_DIR) & 0x80u) ? 1u : 0u);
    bool changed = change_bit != state->last_change_bit;
    if (changed) state->last_change_bit = change_bit;

    if (state->ejected) {
        if (!changed) return MEDIA_ABSENT;
        state->ejected = false;
    }

    if (state->media_valid && !changed) {
        if (state->motor_on) {
            state->motor_idle_polls++;
            if (state->motor_idle_polls >= FDC_MOTOR_IDLE_POLLS) {
                fdc_motor_off(state);
            }
        }
        return MEDIA_PRESENT;
    }

    if (!changed && state->absent_probe_delay > 0) {
        state->absent_probe_delay--;
        return state->media_valid ? MEDIA_PRESENT : MEDIA_ABSENT;
    }
    state->absent_probe_delay = 2;

    bool old_media_valid = state->media_valid;
    uint32_t old_sectors = state->total_sectors;
    media_state_t media = fdc_detect_media_locked(drive);
    bool media_changed = changed || !old_media_valid ||
                         old_sectors != state->total_sectors;
    fdc_update_block_capacity(state, media, media_changed);
    if (media != MEDIA_PRESENT) fdc_motor_off(state);
    return media;
}

int fdc_eject(int drive) {
    if (drive < 0 || drive >= 2 || !g_fdc_drives[drive].hardware_present) {
        return -1;
    }
    fdc_drive_t* state = &g_fdc_drives[drive];
    fdc_motor_off(state);
    state->media_valid = false;
    state->total_sectors = 0;
    state->current_cylinder = -1;
    state->ejected = true;
    state->last_change_bit =
        (uint8_t)((inb(FDC_DIR) & 0x80u) ? 1u : 0u);
    state->absent_probe_delay = 0;
    if (state->block) block_set_capacity(state->block, 0, FDC_SECTOR_SIZE);
    log_printf("[FDC ] %c: eject requested; waiting for media change\n",
               'A' + drive);
    return 0;
}

int fdc_read_sectors(int drive, unsigned int lba, unsigned int count,
                     unsigned char* buffer) {
    if (drive < 0 || drive >= 2 || !buffer || count == 0) return -1;
    fdc_drive_t* state = &g_fdc_drives[drive];
    if (!state->media_valid ||
        lba >= state->total_sectors ||
        count > state->total_sectors - lba) {
        return -1;
    }

    unsigned int completed = 0;
    while (completed < count) {
        uint32_t current_lba = lba + completed;
        uint32_t track_remain = state->sectors_per_track -
            (current_lba % state->sectors_per_track);
        uint32_t batch = count - completed;
        if (batch > FDC_MAX_BATCH_SECTORS) batch = FDC_MAX_BATCH_SECTORS;
        if (batch > track_remain) batch = track_remain;
        if (fdc_transfer_chunk(drive, false, current_lba, batch, NULL,
                               buffer + completed * FDC_SECTOR_SIZE) != 0) {
            state->media_valid = false;
            state->current_cylinder = -1;
            state->absent_probe_delay = 0;
            return -1;
        }
        completed += batch;
    }
    return 0;
}

int fdc_write_sectors(int drive, unsigned int lba, unsigned int count,
                      const unsigned char* buffer) {
    if (drive < 0 || drive >= 2 || !buffer || count == 0) return -1;
    fdc_drive_t* state = &g_fdc_drives[drive];
    if (!state->media_valid ||
        lba >= state->total_sectors ||
        count > state->total_sectors - lba) {
        return -1;
    }

    unsigned int completed = 0;
    while (completed < count) {
        uint32_t current_lba = lba + completed;
        uint32_t track_remain = state->sectors_per_track -
            (current_lba % state->sectors_per_track);
        uint32_t batch = count - completed;
        if (batch > FDC_MAX_BATCH_SECTORS) batch = FDC_MAX_BATCH_SECTORS;
        if (batch > track_remain) batch = track_remain;
        if (fdc_transfer_chunk(drive, true, current_lba, batch,
                               buffer + completed * FDC_SECTOR_SIZE,
                               NULL) != 0) {
            state->media_valid = false;
            state->current_cylinder = -1;
            state->absent_probe_delay = 0;
            return -1;
        }
        completed += batch;
    }
    return 0;
}
