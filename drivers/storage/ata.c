#include "ata.h"
#include "blkdev.h"
#include "log.h"
#include "port.h"
#include "string.h"

extern void boot_screen_tick(void);

#define ATA_MAX_CHANNELS 2
#define ATA_DRIVES_PER_CHANNEL 2

#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT0 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_HDDEVSEL 6
#define ATA_REG_COMMAND 7
#define ATA_REG_STATUS 7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF 0x20
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_READ_PIO_EXT 0x24
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET 0xA0

typedef struct {
    uint16_t base;
    uint16_t control;
    uint8_t channel;
    uint8_t slave;
    bool present;
    bool packet;
    uint64_t sectors;
    uint32_t sector_size;
    char model[48];
} ata_drive_t;

static ata_drive_t g_ata_drives[ATA_MAX_CHANNELS * ATA_DRIVES_PER_CHANNEL];
static uint8_t g_ata_count;

static void ata_delay400(ata_drive_t* drive) {
    (void)inb(drive->control);
    (void)inb(drive->control);
    (void)inb(drive->control);
    (void)inb(drive->control);
}

static uint8_t ata_status(ata_drive_t* drive) {
    for (int index = 0; index < 4; index++) (void)inb(drive->control);
    return inb(drive->base + ATA_REG_STATUS);
}

static int ata_wait_status(ata_drive_t* drive, uint8_t wanted,
                           uint8_t forbidden, uint32_t timeout) {
    for (uint32_t spin = 0; spin < timeout; spin++) {
        uint8_t status = ata_status(drive);
        if (status & ATA_SR_BSY) {
            if ((spin & 0xFFFF) == 0) boot_screen_tick();
            __asm__ volatile ("pause");
            continue;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if ((status & forbidden) == 0 && (wanted == 0 ||
                                           (status & wanted) == wanted)) {
            return 0;
        }
        if ((spin & 0xFFFF) == 0) boot_screen_tick();
        __asm__ volatile ("pause");
    }
    return -1;
}

static void ata_select(ata_drive_t* drive, uint8_t extra) {
    outb(drive->base + ATA_REG_HDDEVSEL,
         (uint8_t)(0xA0 | (drive->slave ? 0x10 : 0) | extra));
    ata_delay400(drive);
}

static void ata_read_words(uint16_t port, void* buffer, uint32_t words) {
    __asm__ volatile ("cld; rep insw"
                      : "+D"(buffer), "+c"(words)
                      : "d"(port)
                      : "memory");
}

static void ata_write_words(uint16_t port, const void* buffer,
                            uint32_t words) {
    __asm__ volatile ("cld; rep outsw"
                      : "+S"(buffer), "+c"(words)
                      : "d"(port)
                      : "memory");
}

static void ata_identify_string(char* output, uint16_t capacity,
                                const uint16_t* identify, int first,
                                int count) {
    int out = 0;
    for (int word = first; word < first + count && out + 1 < capacity; word++) {
        uint16_t value = identify[word];
        char high = (char)(value >> 8);
        char low = (char)value;
        if (high && out + 1 < capacity) output[out++] = high;
        if (low && out + 1 < capacity) output[out++] = low;
    }
    while (out > 0 && output[out - 1] == ' ') out--;
    output[out] = '\0';
}

static bool identify_drive(ata_drive_t* drive) {
    outb(drive->control, 0x02);
    ata_select(drive, 0);
    outb(drive->base + ATA_REG_SECCOUNT0, 0);
    outb(drive->base + ATA_REG_LBA0, 0);
    outb(drive->base + ATA_REG_LBA1, 0);
    outb(drive->base + ATA_REG_LBA2, 0);
    uint8_t status = ata_status(drive);
    if (status == 0 || status == 0xFF) return false;

    outb(drive->base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        status = ata_status(drive);
        if (!(status & ATA_SR_BSY)) break;
    }
    if (status == 0 || (status & ATA_SR_BSY)) return false;

    uint8_t low = inb(drive->base + ATA_REG_LBA1);
    uint8_t high = inb(drive->base + ATA_REG_LBA2);
    uint8_t command = ATA_CMD_IDENTIFY;
    if (low == 0x14 && high == 0xEB) {
        drive->packet = true;
        command = ATA_CMD_IDENTIFY_PACKET;
    } else if (low == 0x3C && high == 0xC3) {
        drive->packet = true;
        command = ATA_CMD_IDENTIFY_PACKET;
    } else if (low || high) {
        return false;
    }
    if (drive->packet) {
        outb(drive->base + ATA_REG_COMMAND, command);
        if (ata_wait_status(drive, ATA_SR_DRQ, ATA_SR_BSY, 1000000) != 0) {
            return false;
        }
    } else if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        return false;
    } else if (ata_wait_status(drive, ATA_SR_DRQ, ATA_SR_BSY,
                               1000000) != 0) {
        return false;
    }

    uint16_t identify[256];
    ata_read_words(drive->base + ATA_REG_DATA, identify, 256);
    ata_identify_string(drive->model, sizeof(drive->model), identify, 27, 20);
    if (drive->packet) {
        drive->sector_size = 2048;
        drive->sectors = 0;
    } else {
        uint32_t lba28 = ((uint32_t)identify[61] << 16) | identify[60];
        uint64_t lba48 = (uint64_t)lba28;
        if (identify[83] & (1u << 10)) {
            lba48 = ((uint64_t)identify[103] << 48) |
                    ((uint64_t)identify[102] << 32) |
                    ((uint64_t)identify[101] << 16) | identify[100];
        }
        drive->sectors = lba48 ? lba48 : lba28;
        drive->sector_size = 512;
    }
    drive->present = true;
    return true;
}

static void ata_program_lba(ata_drive_t* drive, uint64_t lba,
                            uint32_t count, bool lba48) {
    if (lba48) {
        outb(drive->base + ATA_REG_SECCOUNT0, (uint8_t)(count >> 8));
        outb(drive->base + ATA_REG_LBA0, (uint8_t)(lba >> 24));
        outb(drive->base + ATA_REG_LBA1, (uint8_t)(lba >> 32));
        outb(drive->base + ATA_REG_LBA2, (uint8_t)(lba >> 40));
    }
    outb(drive->base + ATA_REG_SECCOUNT0, (uint8_t)count);
    outb(drive->base + ATA_REG_LBA0, (uint8_t)lba);
    outb(drive->base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(drive->base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
}

static int ata_read_blocks(blkdev_t* block, uint64_t lba, uint32_t count,
                           void* buffer) {
    ata_drive_t* drive = block ? (ata_drive_t*)block->priv : NULL;
    if (!drive || !drive->present || drive->packet || !buffer) return -1;
    uint8_t* output = (uint8_t*)buffer;
    while (count) {
        uint32_t batch = count > 128 ? 128 : count;
        bool extended = lba + batch > 0x0FFFFFFF;
        uint8_t head_extra = extended ? 0x40 : (uint8_t)(0x40 | ((lba >> 24) & 0x0F));
        ata_select(drive, head_extra);
        if (ata_wait_status(drive, 0, ATA_SR_BSY, 1000000) != 0) return -1;
        ata_program_lba(drive, lba, batch, extended);
        outb(drive->base + ATA_REG_COMMAND,
             extended ? ATA_CMD_READ_PIO_EXT : ATA_CMD_READ_PIO);
        for (uint32_t sector = 0; sector < batch; sector++) {
            if (ata_wait_status(drive, ATA_SR_DRQ, ATA_SR_BSY, 1000000) != 0) {
                return -1;
            }
            ata_read_words(drive->base + ATA_REG_DATA, output, 256);
            output += 512;
        }
        lba += batch;
        count -= batch;
    }
    return 0;
}

static int ata_write_blocks(blkdev_t* block, uint64_t lba, uint32_t count,
                            const void* buffer) {
    ata_drive_t* drive = block ? (ata_drive_t*)block->priv : NULL;
    if (!drive || !drive->present || drive->packet || !buffer) return -1;
    const uint8_t* input = (const uint8_t*)buffer;
    while (count) {
        uint32_t batch = count > 128 ? 128 : count;
        bool extended = lba + batch > 0x0FFFFFFF;
        uint8_t head_extra = extended ? 0x40 : (uint8_t)(0x40 | ((lba >> 24) & 0x0F));
        ata_select(drive, head_extra);
        if (ata_wait_status(drive, 0, ATA_SR_BSY, 1000000) != 0) return -1;
        ata_program_lba(drive, lba, batch, extended);
        outb(drive->base + ATA_REG_COMMAND,
             extended ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_WRITE_PIO);
        for (uint32_t sector = 0; sector < batch; sector++) {
            if (ata_wait_status(drive, ATA_SR_DRQ, ATA_SR_BSY, 1000000) != 0) {
                return -1;
            }
            ata_write_words(drive->base + ATA_REG_DATA, input, 256);
            input += 512;
            ata_delay400(drive);
        }
        lba += batch;
        count -= batch;
    }
    outb(drive->base + ATA_REG_COMMAND, 0xE7);
    (void)ata_wait_status(drive, 0, ATA_SR_BSY, 1000000);
    return 0;
}

static uint32_t be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static void write_be32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static int atapi_packet(ata_drive_t* drive, const uint8_t* packet,
                        void* buffer, uint32_t bytes, bool input,
                        bool quiet) {
    if (!drive || !packet || !drive->packet) return -1;
    if (bytes > 0xFFFEu) return -1;
    ata_select(drive, 0);
    outb(drive->base + ATA_REG_FEATURES, 0);
    outb(drive->base + ATA_REG_SECCOUNT0, 0);
    outb(drive->base + ATA_REG_LBA0, 0);
    outb(drive->base + ATA_REG_LBA1, (uint8_t)bytes);
    outb(drive->base + ATA_REG_LBA2, (uint8_t)(bytes >> 8));
    outb(drive->base + ATA_REG_COMMAND, ATA_CMD_PACKET);
    if (ata_wait_status(drive, ATA_SR_DRQ, ATA_SR_BSY, 1000000) != 0) {
        if (!quiet) {
            log_printf("[ATA ] ATAPI packet command: status=%02x error=%02x\n",
                       inb(drive->base + ATA_REG_STATUS),
                       inb(drive->base + ATA_REG_ERROR));
        }
        return -1;
    }
    ata_write_words(drive->base + ATA_REG_DATA, packet, 6);
    ata_delay400(drive);
    if (bytes) {
        if (ata_wait_status(drive, ATA_SR_DRQ, ATA_SR_BSY, 1000000) != 0) {
            if (!quiet) {
                log_printf("[ATA ] ATAPI data wait: status=%02x error=%02x "
                           "bytes=%u\n",
                           inb(drive->base + ATA_REG_STATUS),
                           inb(drive->base + ATA_REG_ERROR), bytes);
            }
            return -1;
        }
        if (input) ata_read_words(drive->base + ATA_REG_DATA, buffer,
                                  bytes / 2);
        else ata_write_words(drive->base + ATA_REG_DATA, buffer, bytes / 2);
    }
    if (ata_wait_status(drive, 0, ATA_SR_BSY, 1000000) != 0) {
        if (!quiet) {
            log_printf("[ATA ] ATAPI completion: status=%02x error=%02x\n",
                       inb(drive->base + ATA_REG_STATUS),
                       inb(drive->base + ATA_REG_ERROR));
        }
        return -1;
    }
    return 0;
}

static int atapi_probe_capacity(ata_drive_t* drive) {
    uint8_t packet[12];
    uint8_t response[8];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x25;
    if (atapi_packet(drive, packet, response, sizeof(response), true,
                     true) != 0) {
        return -1;
    }
    uint32_t last_lba = be32(response);
    uint32_t block_size = ((uint32_t)response[4] << 24) |
                          ((uint32_t)response[5] << 16) |
                          ((uint32_t)response[6] << 8) | response[7];
    if (!block_size || block_size > 4096) block_size = 2048;
    drive->sector_size = block_size;
    drive->sectors = (uint64_t)last_lba + 1;
    return 0;
}

static int atapi_read_blocks(blkdev_t* block, uint64_t lba, uint32_t count,
                             void* buffer) {
    ata_drive_t* drive = block ? (ata_drive_t*)block->priv : NULL;
    if (!drive || !drive->present || !drive->packet || !buffer) return -1;
    uint8_t* output = (uint8_t*)buffer;
    while (count) {
        uint32_t batch = count > 16 ? 16 : count;
        uint8_t packet[12];
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x28;
        write_be32(packet + 2, (uint32_t)lba);
        packet[7] = (uint8_t)(batch >> 8);
        packet[8] = (uint8_t)batch;
        uint32_t bytes = batch * drive->sector_size;
        if (atapi_packet(drive, packet, output, bytes, true, false) != 0) {
            return -1;
        }
        output += bytes;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static media_state_t atapi_check_media(blkdev_t* block) {
    ata_drive_t* drive = block ? (ata_drive_t*)block->priv : NULL;
    if (!drive || !drive->packet) return MEDIA_ERROR;

    uint8_t packet[12];
    memset(packet, 0, sizeof(packet));
    if (atapi_packet(drive, packet, NULL, 0, true, true) != 0) {
        if (block->total_sectors != 0) {
            drive->sectors = 0;
            block_set_capacity(block, 0, drive->sector_size);
            log_printf("[ATA ] ATAPI tray opened or medium removed\n");
        }
        return MEDIA_ABSENT;
    }

    uint64_t old_sectors = drive->sectors;
    uint32_t old_size = drive->sector_size;
    if (atapi_probe_capacity(drive) != 0) {
        if (block->total_sectors != 0) {
            drive->sectors = 0;
            block_set_capacity(block, 0, drive->sector_size);
        }
        return MEDIA_ABSENT;
    }
    if (old_sectors != drive->sectors || old_size != drive->sector_size ||
        block->total_sectors != drive->sectors) {
        block_set_capacity(block, drive->sectors, drive->sector_size);
        log_printf("[ATA ] ATAPI medium loaded: %llu x %u-byte sectors\n",
                   (unsigned long long)drive->sectors, drive->sector_size);
    } else {
        block->media_state = MEDIA_PRESENT;
    }
    return MEDIA_PRESENT;
}

int ata_atapi_eject(blkdev_t* block) {
    ata_drive_t* drive = block ? (ata_drive_t*)block->priv : NULL;
    if (!drive || !drive->packet) return -1;
    uint8_t packet[12];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x1E;
    (void)atapi_packet(drive, packet, NULL, 0, true, true);
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x1B;
    packet[4] = 0x02;
    int result = atapi_packet(drive, packet, NULL, 0, true, false);
    if (result == 0) {
        drive->sectors = 0;
        block_set_capacity(block, 0, drive->sector_size ? drive->sector_size :
                                                         2048);
        log_printf("[ATA ] ATAPI eject requested for %s\n", block->name);
    }
    return result;
}

static void probe_channel(uint16_t base, uint16_t control, uint8_t channel) {
    for (uint8_t slave = 0; slave < 2; slave++) {
        if (g_ata_count >= ATA_MAX_CHANNELS * ATA_DRIVES_PER_CHANNEL) return;
        ata_drive_t* drive = &g_ata_drives[g_ata_count];
        memset(drive, 0, sizeof(*drive));
        drive->base = base;
        drive->control = control;
        drive->channel = channel;
        drive->slave = slave;
        if (!identify_drive(drive)) continue;
        if (drive->packet) (void)atapi_probe_capacity(drive);
        int unit = channel * 2 + slave;
        blkdev_t* block = block_register(
            drive->packet ? "ata-atapi" : "ata-pio",
            drive->packet ? BLK_TYPE_ATAPI : BLK_TYPE_ATA, unit,
            drive->sector_size, drive->sectors, drive->packet,
            drive->packet,
            drive->packet ? atapi_read_blocks : ata_read_blocks,
            drive->packet ? NULL : ata_write_blocks,
            drive->packet ? atapi_check_media : NULL, drive);
        if (block) {
            memcpy(block->model, drive->model, sizeof(block->model));
            log_printf("[ATA ] %s %u:%u %s sectors=%llu\n",
                       drive->packet ? "ATAPI" : "ATA", channel, slave,
                       drive->model, (unsigned long long)drive->sectors);
        }
        g_ata_count++;
    }
}

void ata_init(void) {
    g_ata_count = 0;
    memset(g_ata_drives, 0, sizeof(g_ata_drives));
    probe_channel(0x1F0, 0x3F6, 0);
    probe_channel(0x170, 0x376, 1);
}
