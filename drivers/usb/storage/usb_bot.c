#include "usb_storage.h"
#include "blkdev.h"
#include "log.h"
#include "string.h"

#define USB_BOT_MAX_DEVICES 4
#define USB_BOT_CBW_SIGNATURE 0x43425355u
#define USB_BOT_CSW_SIGNATURE 0x53425355u
#define USB_BOT_DATA_CHUNK 4096u
#define USB_BOT_CMD_OK             0
#define USB_BOT_CMD_ERROR         -1
#define USB_BOT_CMD_CHECK_COND    -2

typedef struct {
    bool used;
    bool is_optical;
    bool io_fault;
    uint8_t consec_io_errors;
    uint8_t last_sense_key;
    uint8_t last_asc;
    uint8_t last_ascq;
    usb_device_t* device;
    usb_endpoint_t* bulk_in;
    usb_endpoint_t* bulk_out;
    uint32_t tag;
    uint32_t sector_size;
    uint64_t sector_count;
    blkdev_t* block;
    uint8_t buffer[32768] __attribute__((aligned(16)));
} usb_bot_device_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t command[16];
} __attribute__((packed)) bot_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed)) bot_csw_t;

static usb_bot_device_t g_bots[USB_BOT_MAX_DEVICES];
static int g_bot_count;
static int g_next_bot_unit;
static uint32_t g_bot_generation = 1;

uint32_t usb_storage_get_generation(void) {
    return g_bot_generation;
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

static int bot_bulk(usb_bot_device_t* bot, usb_endpoint_t* endpoint,
                    void* data, uint32_t length, int direction) {
    if (!bot || !bot->device || !bot->device->hci ||
        !bot->device->hci->bulk) {
        return -1;
    }
    uint8_t* bytes = (uint8_t*)data;
    uint32_t transferred = 0;
    while (transferred < length) {
        uint32_t chunk = length - transferred;
        if (chunk > USB_BOT_DATA_CHUNK) chunk = USB_BOT_DATA_CHUNK;
        int result = bot->device->hci->bulk(
            bot->device->hci, bot->device, endpoint, bytes + transferred,
            (int)chunk, direction);
        if (result < 0) return -1;
        transferred += (uint32_t)result;
        if ((uint32_t)result < chunk) break;
    }
    return (int)transferred;
}

static int bot_reset_recovery(usb_bot_device_t* bot) {
    if (!bot || !bot->device) return -1;
    int result = usb_control_msg(
        bot->device, USB_DIR_OUT | USB_RT_CLASS | USB_RT_INTERFACE,
        0xFF, 0, bot->device->interface_number, NULL, 0);
    usb_delay_ms(10);
    if (bot->bulk_in) (void)usb_clear_halt(bot->device, bot->bulk_in);
    if (bot->bulk_out) (void)usb_clear_halt(bot->device, bot->bulk_out);
    return result;
}

static int bot_command_raw(usb_bot_device_t* bot, const uint8_t* command,
                           uint8_t command_length, void* data,
                           uint32_t data_length, int direction) {
    if (!bot || !bot->device || !bot->device->hci ||
        !command || command_length > 16 || data_length > sizeof(bot->buffer)) {
        return USB_BOT_CMD_ERROR;
    }
    bot_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_BOT_CBW_SIGNATURE;
    cbw.tag = ++bot->tag;
    cbw.data_length = data_length;
    cbw.flags = direction == USB_DIR_IN ? 0x80 : 0;
    cbw.cb_length = command_length;
    memcpy(cbw.command, command, command_length);
    if (bot_bulk(bot, bot->bulk_out, &cbw, sizeof(cbw), USB_DIR_OUT) !=
        (int)sizeof(cbw)) {
        log_printf("[USBMS] CBW send failed tag=%u\n", cbw.tag);
        (void)bot_reset_recovery(bot);
        return USB_BOT_CMD_ERROR;
    }

    uint32_t transferred = 0;
    bool data_stalled = false;
    if (data_length) {
        if (direction == USB_DIR_OUT) {
            memcpy(bot->buffer, data, data_length);
        } else {
            memset(bot->buffer, 0, data_length);
        }
        usb_endpoint_t* data_ep =
            (direction == USB_DIR_IN) ? bot->bulk_in : bot->bulk_out;
        int bulk_result = bot_bulk(
            bot, data_ep, bot->buffer, data_length, direction);
        if (bulk_result < 0) {
            /* Per USB BOT spec 6.7.2/6.7.3: clear STALL on data pipe then read CSW */
            (void)usb_clear_halt(bot->device, data_ep);
            data_stalled = true;
        } else {
            /*
             * A short bulk transfer is legal for BOT.  The CSW carries the byte
             * residue and must still be consumed before the next command.
             */
            transferred = (uint32_t)bulk_result;
            if (transferred > data_length) transferred = data_length;
        }
    }

    bot_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    int csw_result = bot_bulk(bot, bot->bulk_in, &csw, sizeof(csw),
                              USB_DIR_IN);
    if (csw_result < 0) {
        (void)usb_clear_halt(bot->device, bot->bulk_in);
        csw_result = bot_bulk(bot, bot->bulk_in, &csw, sizeof(csw),
                              USB_DIR_IN);
    }
    if (csw_result != (int)sizeof(csw) ||
        csw.signature != USB_BOT_CSW_SIGNATURE || csw.tag != cbw.tag ||
        csw.status >= 2 || csw.residue > data_length) {
        log_printf("[USBMS] CSW failure tag=%u got_tag=%u signature=%08x status=%u\n",
                   cbw.tag, csw.tag, csw.signature, csw.status);
        (void)bot_reset_recovery(bot);
        return USB_BOT_CMD_ERROR;
    }
    if (csw.status == 1) {
        return USB_BOT_CMD_CHECK_COND;
    }
    if (data_stalled) {
        return USB_BOT_CMD_ERROR;
    }
    if (data_length && direction == USB_DIR_IN) {
        uint32_t actual = data_length - csw.residue;
        if (actual > transferred) actual = transferred;
        memcpy(data, bot->buffer, actual);
    }
    return USB_BOT_CMD_OK;
}

static int scsi_request_sense(usb_bot_device_t* bot, uint8_t* sense_key,
                              uint8_t* asc, uint8_t* ascq) {
    uint8_t cmd[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));
    if (bot_command_raw(bot, cmd, sizeof(cmd), sense,
                        sizeof(sense), USB_DIR_IN) != USB_BOT_CMD_OK) {
        return -1;
    }
    if (sense_key) *sense_key = sense[2] & 0x0Fu;
    if (asc) *asc = sense[12];
    if (ascq) *ascq = sense[13];
    return 0;
}

static int bot_command(usb_bot_device_t* bot, const uint8_t* command,
                       uint8_t command_length, void* data,
                       uint32_t data_length, int direction) {
    if (!bot || !bot->device || !bot->device->hci || bot->io_fault) {
        return -1;
    }
    bot->last_sense_key = 0;
    bot->last_asc = 0;
    bot->last_ascq = 0;

    for (int retry = 0; retry < 4; retry++) {
        int res = bot_command_raw(bot, command, command_length, data,
                                  data_length, direction);
        if (res == USB_BOT_CMD_OK) {
            bot->consec_io_errors = 0;
            return 0;
        }
        if (res == USB_BOT_CMD_CHECK_COND) {
            uint8_t key = 0, asc = 0, ascq = 0;
            if (scsi_request_sense(bot, &key, &asc, &ascq) == 0) {
                bot->last_sense_key = key;
                bot->last_asc = asc;
                bot->last_ascq = ascq;
                if (key == 0x02 && asc == 0x3A) {
                    if (bot->block && bot->block->media_state != MEDIA_ABSENT) {
                        bot->block->media_state = MEDIA_ABSENT;
                        bot->block->media_generation++;
                    }
                    log_printf("[USBMS] Medium not present on cmd=%02x\n",
                               command[0]);
                    return -1;
                }
                if (key == 0x03 || key == 0x05) {
                    log_printf("[USBMS] SCSI sense key=%02x asc=%02x ascq=%02x cmd=%02x\n",
                               key, asc, ascq, command[0]);
                    return -1;
                }
                if (key == 0x00 || key == 0x01 || key == 0x06 ||
                    (key == 0x02 && asc == 0x04)) {
                    usb_delay_ms(key == 0x02 ? 40 : 5);
                    continue;
                }
                log_printf("[USBMS] SCSI sense key=%02x asc=%02x ascq=%02x cmd=%02x\n",
                           key, asc, ascq, command[0]);
            }
            return -1;
        }
        if (bot->consec_io_errors < UINT8_MAX) bot->consec_io_errors++;
        if (bot->consec_io_errors >= 3) {
            bot->io_fault = true;
            if (bot->block && bot->block->is_removable &&
                bot->block->media_state != MEDIA_ABSENT) {
                bot->block->media_state = MEDIA_ABSENT;
                bot->block->media_generation++;
            }
            log_printf("[USBMS] Bulk I/O breaker open after %u transport errors\n",
                       bot->consec_io_errors);
        }
        return -1;
    }
    return -1;
}

static int scsi_inquiry(usb_bot_device_t* bot) {
    uint8_t command[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t response[36];
    if (bot_command(bot, command, sizeof(command), response,
                    sizeof(response), USB_DIR_IN) != 0) {
        return -1;
    }
    bot->is_optical = ((response[0] & 0x1Fu) == 0x05u);
    char vendor[9];
    char product[17];
    memcpy(vendor, response + 8, 8);
    memcpy(product, response + 16, 16);
    vendor[8] = '\0';
    product[16] = '\0';
    log_printf("[USBMS] SCSI device: %s %s%s\n", vendor, product,
               bot->is_optical ? " (ODD)" : "");
    return 0;
}

static int scsi_capacity(usb_bot_device_t* bot) {
    uint8_t command[10] = { 0x25 };
    uint8_t response[8];
    if (bot_command(bot, command, sizeof(command), response,
                    sizeof(response), USB_DIR_IN) != 0) {
        return -1;
    }
    bot->sector_count = (uint64_t)be32(response) + 1;
    bot->sector_size = be32(response + 4);
    if (!bot->sector_size || bot->sector_size > 4096) {
        bot->sector_size = bot->is_optical ? 2048 : 512;
    }
    return bot->sector_count ? 0 : -1;
}

static int scsi_probe_toc_fallback(usb_bot_device_t* bot) {
    if (!bot || !bot->is_optical) return -1;
    uint8_t command[10];
    uint8_t toc[20];
    memset(command, 0, sizeof(command));
    memset(toc, 0, sizeof(toc));
    command[0] = 0x43;
    command[8] = sizeof(toc);
    if (bot_command(bot, command, sizeof(command), toc, sizeof(toc),
                    USB_DIR_IN) != 0) {
        return -1;
    }
    uint16_t len = (uint16_t)(((uint16_t)toc[0] << 8) | toc[1]);
    if (len < 10) return -1;
    bot->sector_size = 2048;
    bot->sector_count = 1;
    return 0;
}

static int scsi_test_ready(usb_bot_device_t* bot) {
    uint8_t command[6] = { 0x00 };
    return bot_command(bot, command, sizeof(command), NULL, 0, USB_DIR_IN);
}

static int scsi_wait_ready(usb_bot_device_t* bot) {
    for (int attempt = 0; attempt < 25; attempt++) {
        if (scsi_test_ready(bot) == 0) return 0;
        usb_delay_ms(40);
    }
    return -1;
}

static int bot_read_write(blkdev_t* block, uint64_t lba, uint32_t count,
                          void* buffer, int direction) {
    usb_bot_device_t* bot = block ? (usb_bot_device_t*)block->priv : NULL;
    if (!bot || !bot->used || !buffer || !count ||
        !bot->sector_size || bot->sector_size > sizeof(bot->buffer)) {
        return -1;
    }
    uint8_t* bytes = (uint8_t*)buffer;
    uint32_t max_sectors = sizeof(bot->buffer) / bot->sector_size;
    while (count) {
        uint32_t batch = count > max_sectors ? max_sectors : count;
        uint8_t command[10];
        memset(command, 0, sizeof(command));
        command[0] = direction == USB_DIR_IN ? 0x28 : 0x2A;
        write_be32(command + 2, (uint32_t)lba);
        command[7] = (uint8_t)(batch >> 8);
        command[8] = (uint8_t)batch;
        uint32_t bytes_count = batch * bot->sector_size;
        if (bot_command(bot, command, sizeof(command), bytes,
                        bytes_count, direction) != 0) {
            return -1;
        }
        bytes += bytes_count;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static int bot_read_blocks(blkdev_t* block, uint64_t lba, uint32_t count,
                           void* buffer) {
    return bot_read_write(block, lba, count, buffer, USB_DIR_IN);
}

static int bot_write_blocks(blkdev_t* block, uint64_t lba, uint32_t count,
                            const void* buffer) {
    return bot_read_write(block, lba, count, (void*)buffer, USB_DIR_OUT);
}

static int bot_scsi_cmd(blkdev_t* block, const uint8_t* cdb, uint8_t cdb_len,
                        void* buffer, uint32_t buffer_len) {
    usb_bot_device_t* bot = block ? (usb_bot_device_t*)block->priv : NULL;
    if (!bot || !bot->used || !cdb || !cdb_len || cdb_len > 16 ||
        buffer_len > sizeof(bot->buffer)) {
        return -1;
    }
    return bot_command(bot, cdb, cdb_len, buffer, buffer_len, USB_DIR_IN);
}

static media_state_t bot_check_media(blkdev_t* block) {
    usb_bot_device_t* bot = block ? (usb_bot_device_t*)block->priv : NULL;
    if (!bot || !bot->used || !bot->device || !bot->device->hci) {
        return MEDIA_ABSENT;
    }
    bot->io_fault = false;
    uint8_t tur[6] = { 0x00 };
    bool unit_attention = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        int res = bot_command_raw(bot, tur, sizeof(tur), NULL, 0, USB_DIR_IN);
        if (res == USB_BOT_CMD_OK) {
            bot->consec_io_errors = 0;
            if (unit_attention || block->media_state != MEDIA_PRESENT ||
                !block->total_sectors) {
                if (scsi_capacity(bot) == 0 ||
                    scsi_probe_toc_fallback(bot) == 0) {
                    block->total_sectors = bot->sector_count;
                    block->sector_size = bot->sector_size ? bot->sector_size
                                                          : 2048;
                }
            }
            return MEDIA_PRESENT;
        }
        if (res == USB_BOT_CMD_CHECK_COND) {
            uint8_t key = 0, asc = 0, ascq = 0;
            if (scsi_request_sense(bot, &key, &asc, &ascq) == 0) {
                if (key == 0x06) {
                    unit_attention = true;
                    continue;
                }
                if (key == 0x02 && asc == 0x3A) {
                    bot->sector_count = 0;
                    block->total_sectors = 0;
                    return MEDIA_ABSENT;
                }
                if (key == 0x02 && asc == 0x04) {
                    return block->media_state;
                }
            }
        }
        break;
    }
    return MEDIA_ABSENT;
}

int usb_storage_attach(usb_device_t* device) {
    if (!device) return -1;
    int slot = -1;
    for (int i = 0; i < USB_BOT_MAX_DEVICES; i++) {
        if (!g_bots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;
    usb_endpoint_t* bulk_in = NULL;
    usb_endpoint_t* bulk_out = NULL;
    for (uint8_t index = 0; index < device->endpoint_count; index++) {
        usb_endpoint_t* endpoint = &device->endpoints[index];
        if (endpoint->type != USB_EP_TYPE_BULK) continue;
        if (endpoint->interface_class != USB_CLASS_MASS_STORAGE ||
            endpoint->interface_protocol == 0x62) {
            continue;
        }
        if (endpoint->direction == USB_DIR_IN && !bulk_in) bulk_in = endpoint;
        else if (endpoint->direction == USB_DIR_OUT && !bulk_out) bulk_out = endpoint;
    }
    if (!bulk_in || !bulk_out) {
        bulk_in = NULL;
        bulk_out = NULL;
        for (uint8_t index = 0; index < device->endpoint_count; index++) {
            usb_endpoint_t* endpoint = &device->endpoints[index];
            if (endpoint->type != USB_EP_TYPE_BULK) continue;
            if (endpoint->direction == USB_DIR_IN && !bulk_in) bulk_in = endpoint;
            else if (endpoint->direction == USB_DIR_OUT && !bulk_out) bulk_out = endpoint;
        }
    }
    if (!bulk_in || !bulk_out) {
        log_printf("[USBMS] Missing bulk endpoints on device %u\n",
                   device->address);
        return -1;
    }
    usb_bot_device_t* bot = &g_bots[slot];
    memset(bot, 0, sizeof(*bot));
    bot->used = true;
    bot->device = device;
    bot->bulk_in = bulk_in;
    bot->bulk_out = bulk_out;
    if (scsi_inquiry(bot) != 0) {
        log_printf("[USBMS] SCSI INQUIRY failed on device %u\n",
                   device->address);
        memset(bot, 0, sizeof(*bot));
        return -1;
    }
    if (scsi_wait_ready(bot) != 0) {
        if (!bot->is_optical) {
            log_printf("[USBMS] TEST UNIT READY failed on device %u\n",
                       device->address);
            memset(bot, 0, sizeof(*bot));
            return -1;
        }
    }
    int capacity_result = -1;
    for (int attempt = 0; attempt < 6; attempt++) {
        capacity_result = scsi_capacity(bot);
        if (capacity_result == 0) break;
        usb_delay_ms(40);
    }
    if (capacity_result != 0 && bot->is_optical) {
        capacity_result = scsi_probe_toc_fallback(bot);
    }
    if (capacity_result != 0) {
        if (!bot->is_optical) {
            log_printf("[USBMS] READ CAPACITY failed on device %u\n",
                       device->address);
            memset(bot, 0, sizeof(*bot));
            return -1;
        }
        bot->sector_size = 2048;
        bot->sector_count = 0;
    }
    bot->block = block_register(
        "usb-bot", BLK_TYPE_USB, g_next_bot_unit++, bot->sector_size,
        bot->sector_count, bot->is_optical, true, bot_read_blocks,
        bot->is_optical ? NULL : bot_write_blocks, bot_check_media, bot);
    if (!bot->block) {
        memset(bot, 0, sizeof(*bot));
        return -1;
    }
    if (bot->is_optical) {
        bot->block->scsi_cmd = bot_scsi_cmd;
    }
    g_bot_count++;
    g_bot_generation++;
    log_printf("[USBMS] Attached BOT address=%u sectors=%llu size=%u\n",
               device->address, (unsigned long long)bot->sector_count,
               bot->sector_size);
    return 0;
}

void usb_storage_detach(usb_device_t* device) {
    for (int index = 0; index < USB_BOT_MAX_DEVICES; index++) {
        if (g_bots[index].used && g_bots[index].device == device) {
            if (g_bots[index].block) block_unregister(g_bots[index].block);
            memset(&g_bots[index], 0, sizeof(g_bots[0]));
            if (g_bot_count > 0) g_bot_count--;
            g_bot_generation++;
            return;
        }
    }
}
