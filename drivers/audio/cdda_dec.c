#include "cdda_dec.h"
#include "vfs.h"
#include "string.h"

#define CDDA_BATCH_SECTORS 8u

typedef struct {
    int       is_open;
    blkdev_t* dev;
    uint32_t  start_lba;
    uint32_t  sector_count;
    uint32_t  cur_lba;
    uint8_t   sec_buf[CDDA_BATCH_SECTORS * CDDA_SECTOR_SIZE];
    uint32_t  buf_bytes;
    uint32_t  buf_offset;
} cdda_state_t;

static cdda_state_t g_cdda;

static uint32_t cdda_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int cdda_read_toc(blkdev_t* dev, cdda_toc_t* out_toc) {
    if (!dev || !dev->scsi_cmd || !out_toc) return -1;
    memset(out_toc, 0, sizeof(*out_toc));

    uint8_t buf[804];
    memset(buf, 0, sizeof(buf));
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x43; /* READ TOC/PMA/ATIP */
    cdb[1] = 0x00; /* LBA format */
    cdb[2] = 0x00; /* TOC format 0 */
    cdb[6] = 0x00; /* Starting track */
    cdb[7] = (uint8_t)(sizeof(buf) >> 8);
    cdb[8] = (uint8_t)(sizeof(buf) & 0xFFu);

    if (blkdev_scsi_cmd(dev, cdb, 10, buf, sizeof(buf)) != 0) {
        return -1;
    }

    uint16_t toc_len = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    if (toc_len < 2) return -1;
    uint32_t avail_bytes = (uint32_t)toc_len - 2u;
    if (avail_bytes > sizeof(buf) - 4u) avail_bytes = sizeof(buf) - 4u;
    uint32_t desc_count = avail_bytes / 8u;
    if (desc_count < 2u) return -1;
    if (desc_count > 100u) desc_count = 100u;

    for (uint32_t i = 0; i + 1u < desc_count; i++) {
        const uint8_t* d = buf + 4u + i * 8u;
        const uint8_t* next_d = buf + 4u + (i + 1u) * 8u;
        uint8_t track_num = d[2];
        if (track_num == 0 || track_num == 0xAA || track_num > CDDA_MAX_TRACKS) {
            continue;
        }
        uint8_t is_audio = ((d[1] & 0x04u) == 0u) ? 1u : 0u;
        if (!is_audio) continue;

        uint32_t start_lba = cdda_be32(d + 4);
        uint32_t end_lba = cdda_be32(next_d + 4);
        if (end_lba <= start_lba) continue;

        if (out_toc->track_count < CDDA_MAX_TRACKS) {
            cdda_track_t* t = &out_toc->tracks[out_toc->track_count++];
            t->track_num = track_num;
            t->is_audio = 1;
            t->start_lba = start_lba;
            t->sector_count = end_lba - start_lba;
        }
    }

    out_toc->valid = (out_toc->track_count > 0) ? 1 : 0;
    return out_toc->valid ? 0 : -1;
}

int cdda_read_sectors(blkdev_t* dev, uint32_t lba, uint32_t count, void* out_buf) {
    if (!dev || !dev->scsi_cmd || !out_buf || !count) return -1;
    uint8_t* dst = (uint8_t*)out_buf;
    while (count > 0) {
        uint32_t batch = count > CDDA_BATCH_SECTORS ? CDDA_BATCH_SECTORS : count;
        uint32_t bytes = batch * CDDA_SECTOR_SIZE;
        uint8_t cdb[12];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0xBE; /* READ CD */
        cdb[1] = 0x04; /* CD-DA expected sector type */
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[6] = (uint8_t)(batch >> 16);
        cdb[7] = (uint8_t)(batch >> 8);
        cdb[8] = (uint8_t)batch;
        cdb[9] = 0x10; /* User data (2352 bytes/sector) */

        if (blkdev_scsi_cmd(dev, cdb, 12, dst, bytes) != 0) {
            cdb[1] = 0x00; /* Fallback: any sector type */
            if (blkdev_scsi_cmd(dev, cdb, 12, dst, bytes) != 0) {
                return -1;
            }
        }
        dst += bytes;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static int parse_track_number(const char* filepath) {
    if (!filepath) return -1;
    const char* name = filepath;
    for (const char* p = filepath; *p; p++) {
        if (*p == '\\' || *p == '/') name = p + 1;
    }
    int num = 0;
    int digits = 0;
    for (const char* p = name; *p && *p != '.'; p++) {
        if (*p >= '0' && *p <= '9') {
            num = num * 10 + (*p - '0');
            digits++;
        }
    }
    if (digits == 0 || num < 1 || num > CDDA_MAX_TRACKS) return -1;
    return num;
}

int cdda_dec_open(const char* filepath, cdda_info_t* info) {
    cdda_dec_close();
    if (!filepath || !filepath[0] || filepath[1] != ':' || !info) return -1;

    vfs_drive_t* drv = vfs_get_drive(filepath[0]);
    if (!drv || !drv->present || !drv->mounted || !drv->dev) return -1;

    int wanted_track = parse_track_number(filepath);
    if (wanted_track < 1) return -1;

    cdda_toc_t toc;
    if (cdda_read_toc(drv->dev, &toc) != 0) return -1;

    const cdda_track_t* found = NULL;
    for (int i = 0; i < toc.track_count; i++) {
        if ((int)toc.tracks[i].track_num == wanted_track) {
            found = &toc.tracks[i];
            break;
        }
    }
    if (!found && wanted_track <= toc.track_count) {
        found = &toc.tracks[wanted_track - 1];
    }
    if (!found || !found->sector_count) return -1;

    memset(&g_cdda, 0, sizeof(g_cdda));
    g_cdda.is_open = 1;
    g_cdda.dev = drv->dev;
    g_cdda.start_lba = found->start_lba;
    g_cdda.sector_count = found->sector_count;
    g_cdda.cur_lba = found->start_lba;

    memset(info, 0, sizeof(*info));
    info->track_num = found->track_num;
    info->sample_rate = 44100;
    info->channels = 2;
    info->bit_depth = 16;
    info->duration_s = (found->sector_count + (CDDA_SECTORS_PER_SEC / 2u)) /
                       CDDA_SECTORS_PER_SEC;

    char* t = info->title;
    const char* prefix = "Audio CD - Track ";
    int ti = 0;
    while (prefix[ti]) {
        t[ti] = prefix[ti];
        ti++;
    }
    t[ti++] = (char)('0' + (found->track_num / 10));
    t[ti++] = (char)('0' + (found->track_num % 10));
    t[ti] = '\0';
    return 0;
}

int cdda_dec_read_samples(void* out_buf, int max_frames) {
    if (!g_cdda.is_open || !g_cdda.dev || !out_buf || max_frames <= 0) {
        return -1;
    }

    uint32_t end_lba = g_cdda.start_lba + g_cdda.sector_count;
    uint32_t need_bytes = (uint32_t)max_frames * 4u;
    if (need_bytes > 16384u) need_bytes = 16384u;

    uint8_t* dst = (uint8_t*)out_buf;
    uint32_t copied = 0;

    while (copied < need_bytes) {
        if (g_cdda.buf_offset >= g_cdda.buf_bytes) {
            if (g_cdda.cur_lba >= end_lba) break;
            uint32_t rem = end_lba - g_cdda.cur_lba;
            uint32_t batch = rem > CDDA_BATCH_SECTORS ? CDDA_BATCH_SECTORS : rem;
            if (cdda_read_sectors(g_cdda.dev, g_cdda.cur_lba, batch,
                                  g_cdda.sec_buf) != 0) {
                return copied > 0 ? (int)(copied / 4u) : -1;
            }
            g_cdda.cur_lba += batch;
            g_cdda.buf_bytes = batch * CDDA_SECTOR_SIZE;
            g_cdda.buf_offset = 0;
        }
        uint32_t avail = g_cdda.buf_bytes - g_cdda.buf_offset;
        uint32_t take = need_bytes - copied;
        if (take > avail) take = avail;
        memcpy(dst + copied, g_cdda.sec_buf + g_cdda.buf_offset, take);
        g_cdda.buf_offset += take;
        copied += take;
    }

    if (copied == 0) return -1;
    return (int)(copied / 4u);
}

void cdda_dec_seek(unsigned int seconds) {
    if (!g_cdda.is_open) return;
    uint32_t offset_sec = seconds * CDDA_SECTORS_PER_SEC;
    if (offset_sec >= g_cdda.sector_count) {
        offset_sec = g_cdda.sector_count > 0 ? g_cdda.sector_count - 1u : 0u;
    }
    g_cdda.cur_lba = g_cdda.start_lba + offset_sec;
    g_cdda.buf_bytes = 0;
    g_cdda.buf_offset = 0;
}

void cdda_dec_close(void) {
    memset(&g_cdda, 0, sizeof(g_cdda));
}
