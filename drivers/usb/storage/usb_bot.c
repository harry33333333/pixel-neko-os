#include "usb_storage.h"
#include "blkdev.h"
#include "log.h"
#include "string.h"

#define USB_BOT_MAX_DEVICES 4
#define USB_BOT_CBW_SIGNATURE 0x43425355u
#define USB_BOT_CSW_SIGNATURE 0x53425355u
#define USB_BOT_DATA_CHUNK 1024u

typedef struct {
    bool used;
    usb_device_t* device;
    usb_endpoint_t* bulk_in;
    usb_endpoint_t* bulk_out;
    uint32_t tag;
    uint32_t sector_size;
    uint64_t sector_count;
    blkdev_t* block;
    uint8_t buffer[4096] __attribute__((aligned(16)));
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
    return usb_control_msg(
        bot->device, USB_DIR_OUT | USB_RT_CLASS | USB_RT_INTERFACE,
        0xFF, 0, bot->device->interface_number, NULL, 0);
}

static int bot_command(usb_bot_device_t* bot, const uint8_t* command,
                       uint8_t command_length, void* data,
                       uint32_t data_length, int direction) {
    if (!bot || !command || command_length > 16 ||
        data_length > sizeof(bot->buffer)) {
        return -1;
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
        return -1;
    }

    if (data_length) {
        if (direction == USB_DIR_OUT) {
            memcpy(bot->buffer, data, data_length);
        } else {
            memset(bot->buffer, 0, data_length);
        }
        if (bot_bulk(bot, direction == USB_DIR_IN ? bot->bulk_in :
                                                     bot->bulk_out,
                     bot->buffer, data_length, direction) !=
            (int)data_length) {
            (void)bot_reset_recovery(bot);
            return -1;
        }
    }

    bot_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    if (bot_bulk(bot, bot->bulk_in, &csw, sizeof(csw), USB_DIR_IN) !=
        (int)sizeof(csw) ||
        csw.signature != USB_BOT_CSW_SIGNATURE || csw.tag != cbw.tag ||
        csw.status != 0) {
        (void)bot_reset_recovery(bot);
        return -1;
    }
    if (data_length && direction == USB_DIR_IN) {
        memcpy(data, bot->buffer, data_length);
    }
    return 0;
}

static int scsi_inquiry(usb_bot_device_t* bot) {
    uint8_t command[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t response[36];
    if (bot_command(bot, command, sizeof(command), response,
                    sizeof(response), USB_DIR_IN) != 0) {
        return -1;
    }
    char vendor[9];
    char product[17];
    memcpy(vendor, response + 8, 8);
    memcpy(product, response + 16, 16);
    vendor[8] = '\0';
    product[16] = '\0';
    log_printf("[USBMS] SCSI device: %.8s %.16s\n", vendor, product);
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
    if (!bot->sector_size) bot->sector_size = 512;
    return bot->sector_count ? 0 : -1;
}

static int bot_read_write(blkdev_t* block, uint64_t lba, uint32_t count,
                          void* buffer, int direction) {
    usb_bot_device_t* bot = block ? (usb_bot_device_t*)block->priv : NULL;
    if (!bot || !buffer || !count) return -1;
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

int usb_storage_attach(usb_device_t* device) {
    if (!device || g_bot_count >= USB_BOT_MAX_DEVICES) return -1;
    usb_endpoint_t* bulk_in = NULL;
    usb_endpoint_t* bulk_out = NULL;
    for (uint8_t index = 0; index < device->endpoint_count; index++) {
        usb_endpoint_t* endpoint = &device->endpoints[index];
        if (endpoint->type != USB_EP_TYPE_BULK) continue;
        if (endpoint->direction == USB_DIR_IN) bulk_in = endpoint;
        else bulk_out = endpoint;
    }
    if (!bulk_in || !bulk_out) {
        log_printf("[USBMS] Missing bulk endpoints on device %u\n",
                   device->address);
        return -1;
    }
    usb_bot_device_t* bot = &g_bots[g_bot_count];
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
    if (scsi_capacity(bot) != 0) {
        log_printf("[USBMS] READ CAPACITY failed on device %u\n",
                   device->address);
        memset(bot, 0, sizeof(*bot));
        return -1;
    }
    bot->block = block_register(
        "usb-bot", BLK_TYPE_USB, g_bot_count, bot->sector_size,
        bot->sector_count, false, true, bot_read_blocks, bot_write_blocks,
        NULL, bot);
    if (!bot->block) {
        memset(bot, 0, sizeof(*bot));
        return -1;
    }
    g_bot_count++;
    log_printf("[USBMS] Attached BOT address=%u sectors=%llu size=%u\n",
               device->address, (unsigned long long)bot->sector_count,
               bot->sector_size);
    return 0;
}

void usb_storage_detach(usb_device_t* device) {
    for (int index = 0; index < g_bot_count; index++) {
        if (g_bots[index].used && g_bots[index].device == device) {
            if (g_bots[index].block) block_unregister(g_bots[index].block);
            g_bots[index] = g_bots[g_bot_count - 1];
            memset(&g_bots[g_bot_count - 1], 0, sizeof(g_bots[0]));
            g_bot_count--;
            return;
        }
    }
}
