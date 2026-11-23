#ifndef CDDA_DEC_H
#define CDDA_DEC_H

#include <stdint.h>
#include "blkdev.h"

#define CDDA_SECTOR_SIZE 2352u
#define CDDA_SECTORS_PER_SEC 75u
#define CDDA_MAX_TRACKS 99

typedef struct {
    uint8_t  track_num;
    uint8_t  is_audio;
    uint32_t start_lba;
    uint32_t sector_count;
} cdda_track_t;

typedef struct {
    int          valid;
    int          track_count;
    cdda_track_t tracks[CDDA_MAX_TRACKS];
} cdda_toc_t;

typedef struct {
    int  track_num;
    int  sample_rate;
    int  channels;
    int  bit_depth;
    unsigned int duration_s;
    char title[128];
} cdda_info_t;

int cdda_read_toc(blkdev_t* dev, cdda_toc_t* out_toc);
int cdda_read_sectors(blkdev_t* dev, uint32_t lba, uint32_t count, void* out_buf);

int  cdda_dec_open(const char* filepath, cdda_info_t* info);
int  cdda_dec_read_samples(void* out_buf, int max_frames);
void cdda_dec_seek(unsigned int seconds);
void cdda_dec_close(void);

#endif
