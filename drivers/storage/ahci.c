#include "ahci.h"
#include "blkdev.h"
#include "dma.h"
#include "log.h"
#include "pci.h"
#include "string.h"
#include "usb_compat.h"
#include "usb_core.h"

#define AHCI_MAX_PORTS 16
#define AHCI_SECTOR_SIZE 512

#define AHCI_CAP 0x00
#define AHCI_CAP2 0x24
#define AHCI_GHC 0x04
#define AHCI_IS 0x08
#define AHCI_PI 0x0C
#define AHCI_VS 0x10

#define AHCI_PORT_BASE 0x100
#define AHCI_PORT_STRIDE 0x80
#define AHCI_PX_CLB 0x00
#define AHCI_PX_CLBU 0x04
#define AHCI_PX_FB 0x08
#define AHCI_PX_FBU 0x0C
#define AHCI_PX_IS 0x10
#define AHCI_PX_IE 0x14
#define AHCI_PX_CMD 0x18
#define AHCI_PX_TFD 0x20
#define AHCI_PX_SIG 0x24
#define AHCI_PX_SSTS 0x28
#define AHCI_PX_SERR 0x30
#define AHCI_PX_CI 0x38

#define AHCI_GHC_AE 0x80000000u
#define AHCI_GHC_HR 0x00000001u
#define AHCI_PORT_CMD_ST 0x00000001u
#define AHCI_PORT_CMD_FRE 0x00000010u
#define AHCI_PORT_CMD_FR 0x00004000u
#define AHCI_PORT_CMD_CR 0x00008000u
#define AHCI_PORT_TFD_ERR 0x01u
#define AHCI_PORT_SSTS_DET 0x0Fu
#define AHCI_PORT_SIG_ATA 0x00000101u
#define AHCI_PORT_SIG_ATAPI 0xEB140101u

#define AHCI_CMD_READ_DMA_EXT 0x25u
#define AHCI_CMD_WRITE_DMA_EXT 0x35u
#define AHCI_CMD_IDENTIFY 0xECu
#define AHCI_CMD_IDENTIFY_PACKET 0xA1u
#define AHCI_CMD_PACKET 0xA0u

typedef struct {
    volatile uint16_t flags;
    volatile uint16_t prdt_length;
    volatile uint32_t prdbc;
    volatile uint32_t ctba;
    volatile uint32_t ctbau;
    volatile uint32_t reserved[4];
} __attribute__((packed)) ahci_command_header_t;

typedef struct {
    volatile uint64_t data_base;
    volatile uint32_t reserved;
    volatile uint32_t data_byte_count;
} __attribute__((packed)) ahci_prdt_entry_t;

typedef struct {
    uint8_t command_list[1024] __attribute__((aligned(1024)));
    uint8_t received_fis[256] __attribute__((aligned(256)));
    uint8_t command_table[256] __attribute__((aligned(128)));
    uint8_t data[65536] __attribute__((aligned(128)));
} ahci_port_dma_t;

typedef struct ahci_controller {
    volatile uint8_t* mmio;
    const pci_device_t* pci;
    uint32_t implemented_ports;
    uint64_t dma_mask;
    struct ahci_port* ports;
} ahci_controller_t;

typedef struct ahci_port {
    ahci_controller_t* controller;
    uint8_t index;
    uint32_t signature;
    bool packet;
    uint64_t sectors;
    uint32_t sector_size;
    bool initialized;
    char model[48];
    dma_buffer_t dma;
    ahci_port_dma_t* memory;
} ahci_port_t;

static ahci_controller_t g_ahci;

static uint32_t ahci_read(ahci_controller_t* controller, uint32_t offset) {
    return *(volatile uint32_t*)(controller->mmio + offset);
}

static void ahci_write(ahci_controller_t* controller, uint32_t offset,
                       uint32_t value) {
    *(volatile uint32_t*)(controller->mmio + offset) = value;
}

static uint32_t port_register(ahci_port_t* port, uint32_t offset) {
    return ahci_read(port->controller,
                     AHCI_PORT_BASE + port->index * AHCI_PORT_STRIDE + offset);
}

static void port_write(ahci_port_t* port, uint32_t offset, uint32_t value) {
    ahci_write(port->controller,
               AHCI_PORT_BASE + port->index * AHCI_PORT_STRIDE + offset,
               value);
}

static uint64_t port_dma_phys(ahci_port_t* port, const void* pointer) {
    return port->dma.phys_addr +
           (uint64_t)((const uint8_t*)pointer - (uint8_t*)port->memory);
}

static int wait_clear(ahci_port_t* port, uint32_t reg, uint32_t mask,
                      uint32_t timeout_ms) {
    for (uint32_t elapsed = 0; elapsed < timeout_ms * 1000u; elapsed++) {
        if (!(port_register(port, reg) & mask)) return 0;
        __asm__ volatile ("pause");
    }
    return -1;
}

static int port_stop(ahci_port_t* port) {
    port_write(port, AHCI_PX_CMD,
               port_register(port, AHCI_PX_CMD) & ~AHCI_PORT_CMD_ST);
    if (wait_clear(port, AHCI_PX_CMD, AHCI_PORT_CMD_CR, 500) != 0) return -1;
    port_write(port, AHCI_PX_CMD,
               port_register(port, AHCI_PX_CMD) & ~AHCI_PORT_CMD_FRE);
    return wait_clear(port, AHCI_PX_CMD, AHCI_PORT_CMD_FR, 500);
}

static int port_start(ahci_port_t* port) {
    uint64_t clb = port_dma_phys(port, port->memory->command_list);
    uint64_t fis = port_dma_phys(port, port->memory->received_fis);
    port_write(port, AHCI_PX_CLB, (uint32_t)clb);
    port_write(port, AHCI_PX_CLBU, (uint32_t)(clb >> 32));
    port_write(port, AHCI_PX_FB, (uint32_t)fis);
    port_write(port, AHCI_PX_FBU, (uint32_t)(fis >> 32));
    port_write(port, AHCI_PX_SERR, 0xFFFFFFFFu);
    port_write(port, AHCI_PX_IS, 0xFFFFFFFFu);
    port_write(port, AHCI_PX_IE, 0);
    port_write(port, AHCI_PX_CMD,
               port_register(port, AHCI_PX_CMD) |
               AHCI_PORT_CMD_FRE | AHCI_PORT_CMD_ST);
    return 0;
}

static void build_fis(ahci_port_t* port, uint8_t command, uint64_t lba,
                      uint32_t count) {
    uint8_t* fis = port->memory->command_table;
    memset(fis, 0, 64);
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = command;
    fis[3] = port->packet ? 0x01 : 0;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = port->packet ? 0 : 1;
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
}

static int ahci_issue(ahci_port_t* port, uint8_t command, uint64_t lba,
                      uint32_t count, const void* write_data, void* read_data,
                      uint32_t bytes, const uint8_t* packet) {
    if (!port || !port->initialized || bytes > sizeof(port->memory->data)) {
        return -1;
    }
    if (port_stop(port) != 0) return -1;
    memset(port->memory->command_list, 0, 1024);
    memset(port->memory->command_table, 0, sizeof(port->memory->command_table));
    build_fis(port, command, lba, count);
    if (packet) memcpy(port->memory->command_table + 0x40, packet, 16);
    if (write_data && bytes) {
        memcpy(port->memory->data, write_data, bytes);
    } else if (bytes) {
        memset(port->memory->data, 0, bytes);
    }

    ahci_command_header_t* header =
        (ahci_command_header_t*)port->memory->command_list;
    header->flags = (uint16_t)(5u | (1u << 7) |
                               ((bytes && write_data) ? (1u << 6) : 0) |
                               (packet ? (1u << 5) : 0));
    header->prdt_length = bytes ? 1 : 0;
    header->ctba =
        (uint32_t)port_dma_phys(port, port->memory->command_table);
    header->ctbau =
        (uint32_t)(port_dma_phys(port, port->memory->command_table) >> 32);
    if (bytes) {
        ahci_prdt_entry_t* prdt =
            (ahci_prdt_entry_t*)(port->memory->command_table + 128);
        prdt->data_base = port_dma_phys(port, port->memory->data);
        prdt->data_byte_count = bytes - 1;
    }
    if (port_start(port) != 0) return -1;
    dma_sync_for_device(&port->dma);
    port_write(port, AHCI_PX_IS, 0xFFFFFFFFu);
    dma_sync_for_device(&port->dma);
    port_write(port, AHCI_PX_CI, 1);
    int result = -1;
    for (uint32_t elapsed = 0; elapsed < 2000; elapsed++) {
        uint32_t ci = port_register(port, AHCI_PX_CI);
        uint32_t interrupt = port_register(port, AHCI_PX_IS);
        if (!(ci & 1u) || (interrupt & ((1u << 30) | (1u << 29) |
                                        (1u << 28) | (1u << 27)))) {
            uint32_t tfd = port_register(port, AHCI_PX_TFD);
            result = (!(tfd & AHCI_PORT_TFD_ERR) &&
                      !(interrupt & ((1u << 30) | (1u << 29) | (1u << 28) |
                                     (1u << 27)))) ? 0 : -1;
            break;
        }
        usb_delay_ms(1);
    }
    dma_sync_for_cpu(&port->dma);
    port_write(port, AHCI_PX_IS, 0xFFFFFFFFu);
    if (result == 0 && read_data && bytes) {
        memcpy(read_data, port->memory->data, bytes);
    }
    return result;
}

static uint16_t identify_word(const uint16_t* identify, int index) {
    return identify[index];
}

static void identify_string(char* output, unsigned int capacity,
                            const uint16_t* identify) {
    unsigned int out = 0;
    for (int index = 27; index < 47 && out + 1 < capacity; index++) {
        char high = (char)(identify[index] >> 8);
        char low = (char)identify[index];
        if (high && out + 1 < capacity) output[out++] = high;
        if (low && out + 1 < capacity) output[out++] = low;
    }
    while (out && output[out - 1] == ' ') out--;
    output[out] = '\0';
}

static uint32_t be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static void be32_write(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static int ahci_atapi_count(ahci_port_t* port, uint64_t lba,
                            uint32_t count, void* buffer) {
    uint8_t packet[16];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x28;
    be32_write(packet + 2, (uint32_t)lba);
    packet[7] = (uint8_t)(count >> 8);
    packet[8] = (uint8_t)count;
    return ahci_issue(port, AHCI_CMD_PACKET, 0, count, NULL, buffer,
                      count * port->sector_size, packet);
}

static int ahci_read_blocks(blkdev_t* block, uint64_t lba, uint32_t count,
                            void* buffer) {
    ahci_port_t* port = block ? (ahci_port_t*)block->priv : NULL;
    if (!port || !port->initialized || !buffer) return -1;
    uint8_t* output = (uint8_t*)buffer;
    uint32_t max_batch = (uint32_t)(sizeof(port->memory->data) /
                                    port->sector_size);
    while (count) {
        uint32_t batch = count > max_batch ? max_batch : count;
        int result = port->packet
                         ? ahci_atapi_count(port, lba, batch, output)
                         : ahci_issue(port, AHCI_CMD_READ_DMA_EXT, lba, batch,
                                      NULL, output, batch * port->sector_size,
                                      NULL);
        if (result != 0) {
            log_printf("[AHCI] Read failed port=%u lba=%llu count=%u\n",
                       port->index, (unsigned long long)lba, batch);
            return -1;
        }
        output += batch * port->sector_size;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static int ahci_write_blocks(blkdev_t* block, uint64_t lba, uint32_t count,
                             const void* buffer) {
    ahci_port_t* port = block ? (ahci_port_t*)block->priv : NULL;
    if (!port || !port->initialized || port->packet || !buffer) return -1;
    const uint8_t* input = (const uint8_t*)buffer;
    uint32_t max_batch = (uint32_t)(sizeof(port->memory->data) /
                                    port->sector_size);
    while (count) {
        uint32_t batch = count > max_batch ? max_batch : count;
        if (ahci_issue(port, AHCI_CMD_WRITE_DMA_EXT, lba, batch, input, NULL,
                       batch * port->sector_size, NULL) != 0) {
            return -1;
        }
        input += batch * port->sector_size;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static int ahci_probe_port(ahci_port_t* port) {
    uint32_t status = port_register(port, AHCI_PX_SSTS);
    for (int timeout = 0; timeout < 500 &&
                         (status & AHCI_PORT_SSTS_DET) != 3; timeout++) {
        usb_delay_ms(1);
        status = port_register(port, AHCI_PX_SSTS);
    }
    if ((status & AHCI_PORT_SSTS_DET) != 3) {
        port_write(port, 0x2C, 1);
        usb_delay_ms(20);
        for (int timeout = 0; timeout < 1000; timeout++) {
            status = port_register(port, AHCI_PX_SSTS);
            if ((status & AHCI_PORT_SSTS_DET) == 3) break;
            usb_delay_ms(1);
        }
    }
    if ((status & AHCI_PORT_SSTS_DET) != 3) {
        log_printf("[AHCI] Port %u not ready: SSTS=%08x SIG=%08x\n",
                   port->index, status,
                   port_register(port, AHCI_PX_SIG));
        return -1;
    }
    port->signature = port_register(port, AHCI_PX_SIG);
    log_printf("[AHCI] Port %u detected SSTS=%08x SIG=%08x\n",
               port->index, status, port->signature);
    if (port->signature != AHCI_PORT_SIG_ATA &&
        port->signature != AHCI_PORT_SIG_ATAPI) {
        return -1;
    }
    port->packet = port->signature == AHCI_PORT_SIG_ATAPI;
    uint32_t mask = (uint32_t)(ahci_read(port->controller, AHCI_CAP) |
                              ahci_read(port->controller, AHCI_CAP2));
    if (usb_dma_alloc_zero(sizeof(ahci_port_dma_t), port->controller->dma_mask,
                           PAGE_SIZE, &port->dma) != 0) {
        return -1;
    }
    port->memory = (ahci_port_dma_t*)port->dma.virt_addr;
    memset(port->memory, 0, sizeof(*port->memory));
    (void)mask;
    if (port_stop(port) != 0 || port_start(port) != 0) return -1;
    port->initialized = true;

    if (port->packet) {
        static const char fallback[] = "AHCI ATAPI";
        memcpy(port->model, fallback, sizeof(fallback));
        uint8_t packet[12];
        uint8_t response[8];
        memset(packet, 0, sizeof(packet));
        memset(response, 0, sizeof(response));
        packet[0] = 0x25;
        if (ahci_issue(port, AHCI_CMD_PACKET, 0, 0, NULL, response,
                       sizeof(response), packet) != 0) {
            return -1;
        }
        port->sector_size = ((uint32_t)response[4] << 24) |
                            ((uint32_t)response[5] << 16) |
                            ((uint32_t)response[6] << 8) | response[7];
        if (!port->sector_size) port->sector_size = 2048;
        port->sectors = (uint64_t)be32(response) + 1;
    } else {
        uint16_t identify[256];
        memset(identify, 0, sizeof(identify));
        if (ahci_issue(port, AHCI_CMD_IDENTIFY, 0, 1, NULL, identify,
                       sizeof(identify), NULL) != 0) {
            return -1;
        }
        identify_string(port->model, sizeof(port->model), identify);
        port->sector_size = 512;
        port->sectors = ((uint64_t)identify_word(identify, 103) << 48) |
                        ((uint64_t)identify_word(identify, 102) << 32) |
                        ((uint64_t)identify_word(identify, 101) << 16) |
                        identify_word(identify, 100);
        if (!port->sectors) {
            port->sectors = ((uint64_t)identify_word(identify, 61) << 16) |
                            identify_word(identify, 60);
        }
    }
    if (!port->sectors) return -1;
    blkdev_t* block = block_register(
        port->packet ? "ahci-atapi" : "ahci-sata",
        port->packet ? BLK_TYPE_ATAPI : BLK_TYPE_AHCI, port->index,
        port->sector_size, port->sectors, port->packet, false,
        ahci_read_blocks, port->packet ? NULL : ahci_write_blocks, NULL, port);
    if (!block) return -1;
    memcpy(block->model, port->model, sizeof(block->model));
    log_printf("[AHCI] Port %u %s: %s, %llu sectors\n", port->index,
               port->packet ? "ATAPI" : "SATA", port->model,
               (unsigned long long)port->sectors);
    return 0;
}

void ahci_init(void) {
    memset(&g_ahci, 0, sizeof(g_ahci));
    const pci_device_t* pci = pci_find_class(0x01, 0x06);
    if (!pci || pci->prog_if != 0x01 || !pci->bar[5].valid ||
        pci->bar[5].is_io) {
        return;
    }
    g_ahci.pci = pci;
    g_ahci.mmio = (volatile uint8_t*)usb_map_mmio(pci->bar[5].address,
                                                  0x2000);
    if (!g_ahci.mmio) return;
    pci_enable_memory(pci);
    pci_enable_bus_master(pci);
    uint32_t cap = ahci_read(&g_ahci, AHCI_CAP);
    uint32_t cap2 = ahci_read(&g_ahci, AHCI_CAP2);
    g_ahci.dma_mask = (cap & (1u << 31)) || (cap2 & (1u << 0))
                          ? UINT64_MAX : UINT32_MAX;

    uint32_t ghc = ahci_read(&g_ahci, AHCI_GHC);
    ahci_write(&g_ahci, AHCI_GHC, ghc | AHCI_GHC_AE);
    g_ahci.implemented_ports = ahci_read(&g_ahci, AHCI_PI);
    bool link_ready = false;
    for (uint8_t index = 0; index < AHCI_MAX_PORTS; index++) {
        if (!(g_ahci.implemented_ports & (1u << index))) continue;
        uint32_t ssts = ahci_read(
            &g_ahci, AHCI_PORT_BASE + index * AHCI_PORT_STRIDE + AHCI_PX_SSTS);
        if ((ssts & AHCI_PORT_SSTS_DET) == 3) {
            link_ready = true;
            break;
        }
    }
    if (!link_ready) {
        ahci_write(&g_ahci, AHCI_GHC, ghc | AHCI_GHC_AE | AHCI_GHC_HR);
        for (int timeout = 0; timeout < 1000; timeout++) {
            ghc = ahci_read(&g_ahci, AHCI_GHC);
            if (!(ghc & AHCI_GHC_HR)) break;
            usb_delay_ms(1);
        }
        ahci_write(&g_ahci, AHCI_GHC,
                   ahci_read(&g_ahci, AHCI_GHC) | AHCI_GHC_AE);
    }
    ahci_write(&g_ahci, AHCI_IS, 0xFFFFFFFFu);
    log_printf("[AHCI] Controller %04x:%04x version=%x ports=0x%x\n",
               pci->vendor, pci->device, ahci_read(&g_ahci, AHCI_VS),
               g_ahci.implemented_ports);

    static ahci_port_t ports[AHCI_MAX_PORTS];
    memset(ports, 0, sizeof(ports));
    g_ahci.ports = ports;
    for (uint8_t index = 0; index < AHCI_MAX_PORTS; index++) {
        if (!(g_ahci.implemented_ports & (1u << index))) continue;
        ahci_port_t* port = &ports[index];
        port->controller = &g_ahci;
        port->index = index;
        (void)ahci_probe_port(port);
    }
}
