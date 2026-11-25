#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/audio.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/thread.h>

#include "vplayer_mp4.h"
#include "vplayer_h264.h"
#include "vplayer_audio.h"
#include "vplayer_yuv.h"

/* =========================================================================
 * Constants & Pipeline Queue Structures
 * ========================================================================= */

#define VP_CTRL_BAR_H      48
#define VP_VPQ_CAP         96
#define VP_FRAME_RING_MAX  8
#define VP_MAX_PKT_BYTES   (4u * 1024u * 1024u)

#define VP_TR(vp, z, e) ((vp)->zh ? (z) : (e))

typedef struct {
    uint64_t pts_us;
    uint64_t dts_us;
    uint32_t size;
    uint8_t  is_audio;
    uint8_t  is_sync;
    uint16_t pad;
} vp_ring_hdr_t;

/* Byte-addressed circular Stream Ring (Thread 1 -> Thread 2) */
typedef struct {
    uint8_t*    buf;
    size_t      cap;
    size_t      head;
    size_t      tail;
    size_t      used;
    int         eof;
    int         flushing;
    pxe_mutex_t mtx;
    pxe_cond_t  not_empty;
    pxe_cond_t  not_full;
} vp_stream_ring_t;

/* Video Packet Queue (Thread 2 -> Thread 3) */
typedef struct {
    uint8_t* data;
    uint32_t size;
    uint64_t pts_us;
    uint8_t  is_sync;
    uint8_t  prepend_sps;
} vp_vpkt_t;

typedef struct {
    vp_vpkt_t   items[VP_VPQ_CAP];
    int         head;
    int         tail;
    int         count;
    int         eof;
    pxe_mutex_t mtx;
    pxe_cond_t  not_empty;
    pxe_cond_t  not_full;
} vp_vpkt_queue_t;

/* Decoded YUV420P Frame Ring (Thread 3 -> Thread 4) */
typedef struct {
    uint8_t* y;
    uint8_t* u;
    uint8_t* v;
    int      w;
    int      h;
    int      y_stride;
    int      uv_stride;
    uint64_t pts_us;
    size_t   alloc_bytes;
} vp_yuv_frame_t;

typedef struct {
    vp_yuv_frame_t slots[VP_FRAME_RING_MAX];
    int            cap;
    int            head;
    int            tail;
    int            count;
    int            eof;
    pxe_mutex_t    mtx;
    pxe_cond_t     not_empty;
    pxe_cond_t     not_full;
} vp_frame_ring_t;

typedef struct {
    int               win_id;
    pxe_win_surface_t surf;
    int               zh;
    int               running;

    /* System info */
    uint32_t          free_ram_mb;
    uint32_t          logical_cpus;
    int               has_audio_hw;
    int               is_cdrom;
    int               is_usb;
    int               volume;
    int               muted;

    /* Current file & index */
    char              file_path[256];
    char              file_name[128];
    int               loaded;
    vp_media_index_t  index;

    /* Playback state */
    volatile int      playing;
    volatile int      paused;
    volatile int      buffering;
    volatile int      stop_threads;
    volatile uint32_t seek_serial;
    volatile uint64_t seek_target_us;
    volatile uint32_t seek_start_sample;
    volatile uint64_t clock_base_us;
    volatile uint64_t wall_start_ns;
    volatile uint64_t pause_pos_us;
    uint64_t          aud_last_played_us;
    uint64_t          aud_last_sample_ns;
    int               audio_stream_opened;
    uint64_t          cur_pos_us;

    /* Buffering metrics */
    size_t            ring_target_bytes;
    size_t            prebuf_target_bytes;

    /* Pipeline queues */
    vp_stream_ring_t  sring;
    vp_vpkt_queue_t   vpq;
    vp_frame_ring_t   fring;

    /* Worker threads */
    int               threads_active;
    pxe_thread_t      t_io;
    pxe_thread_t      t_demux;
    pxe_thread_t      t_vdec;

    /* Last presented frame kept for resize/pause redraw */
    vp_yuv_frame_t    last_frame;
    int               has_last_frame;

    /* UI modals & controls */
    int               open_modal;
    char              open_input[256];
    int               open_len;

    int               err_modal;
    char              err_title[96];
    char              err_msg1[160];
    char              err_msg2[160];

    int               drag_seek;
    int               ui_dirty;
} vplayer_t;

/* =========================================================================
 * Stream Ring Helpers (Thread 1 -> Thread 2)
 * ========================================================================= */

static int sring_init(vp_stream_ring_t* r, size_t cap) {
    memset(r, 0, sizeof(*r));
    r->buf = (uint8_t*)malloc(cap);
    if (!r->buf) return -1;
    r->cap = cap;
    pxe_mutex_init(&r->mtx);
    pxe_cond_init(&r->not_empty);
    pxe_cond_init(&r->not_full);
    return 0;
}

static void sring_destroy(vp_stream_ring_t* r) {
    if (r->buf) { free(r->buf); r->buf = NULL; }
    r->cap = r->head = r->tail = r->used = 0;
}

static void sring_write_raw(vp_stream_ring_t* r, const void* src, size_t n) {
    const uint8_t* p = (const uint8_t*)src;
    size_t first = r->cap - r->tail;
    if (first > n) first = n;
    memcpy(r->buf + r->tail, p, first);
    if (n > first) memcpy(r->buf, p + first, n - first);
    r->tail = (r->tail + n) % r->cap;
    r->used += n;
}

static void sring_read_raw(vp_stream_ring_t* r, void* dst, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    size_t first = r->cap - r->head;
    if (first > n) first = n;
    memcpy(p, r->buf + r->head, first);
    if (n > first) memcpy(p + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->used -= n;
}

static void sring_flush(vp_stream_ring_t* r) {
    pxe_mutex_lock(&r->mtx);
    r->head = r->tail = r->used = 0;
    r->eof = 0;
    pxe_cond_broadcast(&r->not_full);
    pxe_cond_broadcast(&r->not_empty);
    pxe_mutex_unlock(&r->mtx);
}

/* =========================================================================
 * Video Packet Queue Helpers (Thread 2 -> Thread 3)
 * ========================================================================= */

static void vpq_init(vp_vpkt_queue_t* q) {
    memset(q, 0, sizeof(*q));
    pxe_mutex_init(&q->mtx);
    pxe_cond_init(&q->not_empty);
    pxe_cond_init(&q->not_full);
}

static void vpq_flush(vp_vpkt_queue_t* q) {
    pxe_mutex_lock(&q->mtx);
    while (q->count > 0) {
        if (q->items[q->head].data) {
            free(q->items[q->head].data);
            q->items[q->head].data = NULL;
        }
        q->head = (q->head + 1) % VP_VPQ_CAP;
        q->count--;
    }
    q->head = q->tail = 0;
    q->eof = 0;
    pxe_cond_broadcast(&q->not_full);
    pxe_cond_broadcast(&q->not_empty);
    pxe_mutex_unlock(&q->mtx);
}

/* =========================================================================
 * Decoded Frame Ring Helpers (Thread 3 -> Thread 4)
 * ========================================================================= */

static void fring_init(vp_frame_ring_t* f, int cap) {
    memset(f, 0, sizeof(*f));
    if (cap < 2) cap = 2;
    if (cap > VP_FRAME_RING_MAX) cap = VP_FRAME_RING_MAX;
    f->cap = cap;
    pxe_mutex_init(&f->mtx);
    pxe_cond_init(&f->not_empty);
    pxe_cond_init(&f->not_full);
}

static void fring_free_slots(vp_frame_ring_t* f) {
    for (int i = 0; i < VP_FRAME_RING_MAX; i++) {
        if (f->slots[i].y) {
            free(f->slots[i].y);
            f->slots[i].y = f->slots[i].u = f->slots[i].v = NULL;
            f->slots[i].alloc_bytes = 0;
        }
    }
}

static void fring_flush(vp_frame_ring_t* f) {
    pxe_mutex_lock(&f->mtx);
    f->head = f->tail = f->count = 0;
    f->eof = 0;
    pxe_cond_broadcast(&f->not_full);
    pxe_cond_broadcast(&f->not_empty);
    pxe_mutex_unlock(&f->mtx);
}

static int yuv_frame_copy(vp_yuv_frame_t* dst, const vp_h264_frame_t* src) {
    int w = src->width;
    int h = src->height;
    int cw = (w + 1) >> 1;
    int ch = (h + 1) >> 1;
    size_t y_bytes = (size_t)w * (size_t)h;
    size_t c_bytes = (size_t)cw * (size_t)ch;
    size_t need = y_bytes + c_bytes * 2u;
    if (need > dst->alloc_bytes || !dst->y) {
        if (dst->y) free(dst->y);
        dst->y = (uint8_t*)malloc(need);
        if (!dst->y) {
            dst->alloc_bytes = 0;
            return -1;
        }
        dst->alloc_bytes = need;
    }
    dst->u = dst->y + y_bytes;
    dst->v = dst->u + c_bytes;
    dst->w = w;
    dst->h = h;
    dst->y_stride = w;
    dst->uv_stride = cw;
    dst->pts_us = src->pts_us;

    for (int row = 0; row < h; row++) {
        memcpy(dst->y + (size_t)row * (size_t)w,
               src->y_plane + (size_t)row * (size_t)src->y_stride,
               (size_t)w);
    }
    for (int row = 0; row < ch; row++) {
        memcpy(dst->u + (size_t)row * (size_t)cw,
               src->u_plane + (size_t)row * (size_t)src->uv_stride,
               (size_t)cw);
        memcpy(dst->v + (size_t)row * (size_t)cw,
               src->v_plane + (size_t)row * (size_t)src->uv_stride,
               (size_t)cw);
    }
    return 0;
}

/* =========================================================================
 * Thread 1: Async Sequential I/O Pre-read Thread (CD/USB/HDD Anti-Stutter)
 * ========================================================================= */

static void* io_thread_fn(void* arg) {
    vplayer_t* vp = (vplayer_t*)arg;
    int fd = pxe_open(vp->file_path);
    if (fd < 0) {
        pxe_mutex_lock(&vp->sring.mtx);
        vp->sring.eof = 1;
        pxe_cond_broadcast(&vp->sring.not_empty);
        pxe_mutex_unlock(&vp->sring.mtx);
        return NULL;
    }

    const size_t io_blk = vp->is_cdrom ? (512u * 1024u) : (256u * 1024u);
    uint8_t* io_buf = (uint8_t*)malloc(io_blk);
    uint8_t* pkt_tmp = (uint8_t*)malloc(256u * 1024u);
    size_t pkt_cap = pkt_tmp ? (256u * 1024u) : 0u;

    uint64_t buf_file_off = 0;
    size_t   buf_valid = 0;
    uint64_t fd_pos = 0;

    uint32_t cur_serial = vp->seek_serial;
    uint32_t sidx = vp->seek_start_sample;

    while (!vp->stop_threads) {
        if (vp->seek_serial != cur_serial) {
            cur_serial = vp->seek_serial;
            sidx = vp->seek_start_sample;
            buf_valid = 0;
        }

        if (sidx >= vp->index.sample_count) {
            pxe_mutex_lock(&vp->sring.mtx);
            vp->sring.eof = 1;
            pxe_cond_broadcast(&vp->sring.not_empty);
            pxe_mutex_unlock(&vp->sring.mtx);
            vp->buffering = 0;
            /* Wait for seek or stop */
            while (!vp->stop_threads && vp->seek_serial == cur_serial) {
                pxe_sleep_ms(10);
            }
            continue;
        }

        const vp_sample_entry_t* e = &vp->index.samples[sidx];
        if (e->size == 0 || e->size > VP_MAX_PKT_BYTES ||
            sizeof(vp_ring_hdr_t) + e->size >= vp->sring.cap) {
            sidx++;
            continue;
        }

        if (e->size > pkt_cap) {
            uint8_t* nb = (uint8_t*)realloc(pkt_tmp, e->size);
            if (!nb) { sidx++; continue; }
            pkt_tmp = nb;
            pkt_cap = e->size;
        }

        /* Read sample via sequential large-block cache (zero seeks on contiguous mdat) */
        size_t copied = 0;
        int io_err = 0;
        while (copied < e->size && !vp->stop_threads && vp->seek_serial == cur_serial) {
            uint64_t want_off = e->file_offset + copied;
            if (io_buf && buf_valid > 0 &&
                want_off >= buf_file_off && want_off < buf_file_off + buf_valid) {
                size_t in_off = (size_t)(want_off - buf_file_off);
                size_t avail = buf_valid - in_off;
                size_t take = e->size - copied;
                if (take > avail) take = avail;
                memcpy(pkt_tmp + copied, io_buf + in_off, take);
                copied += take;
            } else if (io_buf) {
                if (fd_pos != want_off) {
                    if (pxe_seek(fd, want_off) < 0) { io_err = 1; break; }
                    fd_pos = want_off;
                }
                int r = pxe_read(fd, io_buf, io_blk);
                if (r <= 0) { io_err = 1; break; }
                buf_file_off = want_off;
                buf_valid = (size_t)r;
                fd_pos += (uint64_t)r;
            } else {
                if (fd_pos != want_off) {
                    if (pxe_seek(fd, want_off) < 0) { io_err = 1; break; }
                    fd_pos = want_off;
                }
                int r = pxe_read(fd, pkt_tmp + copied, e->size - copied);
                if (r <= 0) { io_err = 1; break; }
                copied += (size_t)r;
                fd_pos += (uint64_t)r;
            }
        }

        if (vp->stop_threads) break;
        if (vp->seek_serial != cur_serial) continue;
        if (io_err || copied < e->size) {
            sidx++;
            continue;
        }

        vp_ring_hdr_t hdr;
        hdr.pts_us   = e->pts_us;
        hdr.dts_us   = e->dts_us;
        hdr.size     = e->size;
        hdr.is_audio = e->is_audio;
        hdr.is_sync  = e->is_sync;
        hdr.pad      = 0;

        size_t total_need = sizeof(hdr) + e->size;
        pxe_mutex_lock(&vp->sring.mtx);
        while (!vp->stop_threads && vp->seek_serial == cur_serial &&
               vp->sring.cap - vp->sring.used < total_need) {
            if (vp->buffering) vp->buffering = 0;
            pxe_cond_timedwait(&vp->sring.not_full, &vp->sring.mtx, 20000000ULL);
        }
        if (!vp->stop_threads && vp->seek_serial == cur_serial) {
            sring_write_raw(&vp->sring, &hdr, sizeof(hdr));
            sring_write_raw(&vp->sring, pkt_tmp, e->size);
            if (vp->buffering && vp->sring.used >= vp->prebuf_target_bytes) {
                vp->buffering = 0;
            }
            pxe_cond_signal(&vp->sring.not_empty);
        }
        pxe_mutex_unlock(&vp->sring.mtx);

        sidx++;
    }

    if (io_buf) free(io_buf);
    if (pkt_tmp) free(pkt_tmp);
    pxe_close(fd);
    return NULL;
}

/* =========================================================================
 * Thread 2: Demux + Audio Decode Thread (Zero Disk Blocking)
 * ========================================================================= */

static void* demux_audio_thread_fn(void* arg) {
    vplayer_t* vp = (vplayer_t*)arg;
    vp_audio_dec_t* adec = NULL;
    if (vp->index.has_audio) {
        adec = vp_audio_dec_create(vp->index.audio_codec,
                                   vp->index.audio_dsi, vp->index.audio_dsi_len,
                                   vp->index.audio_rate, vp->index.audio_channels,
                                   vp->index.audio_bits);
    }

    int16_t pcm_buf[8192 * 2];
    uint32_t cur_serial = vp->seek_serial;
    int need_sps = 1;

    while (!vp->stop_threads) {
        if (vp->seek_serial != cur_serial) {
            cur_serial = vp->seek_serial;
            need_sps = 1;
            if (adec) vp_audio_dec_reset(adec);
        }

        if (vp->buffering) {
            pxe_sleep_ms(10);
            continue;
        }

        vp_ring_hdr_t hdr;
        uint8_t* payload = NULL;

        pxe_mutex_lock(&vp->sring.mtx);
        while (!vp->stop_threads && vp->seek_serial == cur_serial &&
               vp->sring.used < sizeof(hdr) && !vp->sring.eof) {
            /* CD anti-stutter re-buffering if stream ring drains mid-playback */
            if (vp->is_cdrom && !vp->sring.eof && vp->playing && !vp->paused) {
                vp->buffering = 1;
            }
            pxe_cond_timedwait(&vp->sring.not_empty, &vp->sring.mtx, 15000000ULL);
        }

        if (vp->stop_threads || vp->seek_serial != cur_serial) {
            pxe_mutex_unlock(&vp->sring.mtx);
            continue;
        }

        if (vp->sring.used < sizeof(hdr) && vp->sring.eof) {
            pxe_mutex_unlock(&vp->sring.mtx);
            pxe_mutex_lock(&vp->vpq.mtx);
            vp->vpq.eof = 1;
            pxe_cond_broadcast(&vp->vpq.not_empty);
            pxe_mutex_unlock(&vp->vpq.mtx);
            while (!vp->stop_threads && vp->seek_serial == cur_serial) {
                pxe_sleep_ms(10);
            }
            continue;
        }

        sring_read_raw(&vp->sring, &hdr, sizeof(hdr));
        payload = (uint8_t*)malloc(hdr.size);
        if (payload) {
            sring_read_raw(&vp->sring, payload, hdr.size);
        } else {
            /* Discard on OOM */
            size_t skip = hdr.size;
            vp->sring.head = (vp->sring.head + skip) % vp->sring.cap;
            vp->sring.used -= skip;
        }
        pxe_cond_signal(&vp->sring.not_full);
        pxe_mutex_unlock(&vp->sring.mtx);

        if (!payload) continue;

        if (hdr.is_audio) {
            if (adec && vp->has_audio_hw && vp->audio_stream_opened) {
                /* Wait while paused or while kernel PCM ring has > 260ms queued */
                while (!vp->stop_threads && vp->seek_serial == cur_serial) {
                    if (vp->paused || !vp->playing) {
                        pxe_sleep_ms(10);
                        continue;
                    }
                    pxe_audio_timing_t at;
                    if (pxe_audio_get_timing(&at) == 0 && at.queued_us > 260000u) {
                        pxe_sleep_ms(8);
                        continue;
                    }
                    break;
                }
                if (!vp->stop_threads && vp->seek_serial == cur_serial) {
                    int sr = vp->index.audio_rate;
                    int frames = vp_audio_dec_decode(adec, payload, hdr.size, pcm_buf, 8192, &sr);
                    if (frames > 0) {
                        (void)pxe_audio_feed_pcm(pcm_buf, (uint32_t)frames, sr, 2, 16, 0);
                    }
                }
            }
            free(payload);
        } else {
            /* Route video packet to Thread 3 */
            pxe_mutex_lock(&vp->vpq.mtx);
            while (!vp->stop_threads && vp->seek_serial == cur_serial &&
                   vp->vpq.count >= VP_VPQ_CAP) {
                pxe_cond_timedwait(&vp->vpq.not_full, &vp->vpq.mtx, 15000000ULL);
            }
            if (!vp->stop_threads && vp->seek_serial == cur_serial) {
                vp_vpkt_t* slot = &vp->vpq.items[vp->vpq.tail];
                slot->data        = payload;
                slot->size        = hdr.size;
                slot->pts_us      = hdr.pts_us;
                slot->is_sync     = hdr.is_sync;
                slot->prepend_sps = (uint8_t)(need_sps || hdr.is_sync);
                need_sps = 0;
                vp->vpq.tail = (vp->vpq.tail + 1) % VP_VPQ_CAP;
                vp->vpq.count++;
                pxe_cond_signal(&vp->vpq.not_empty);
            } else {
                free(payload);
            }
            pxe_mutex_unlock(&vp->vpq.mtx);
        }
    }

    if (adec) vp_audio_dec_destroy(adec);
    return NULL;
}

/* =========================================================================
 * Thread 3: Multi-Core OpenH264 Video Decode Thread
 * ========================================================================= */

static void* video_decode_thread_fn(void* arg) {
    vplayer_t* vp = (vplayer_t*)arg;
    /* Scale OpenH264 worker threads to online SMP cores (leaving CPU 0 for compositor) */
    int dec_threads = 1;
    if (vp->logical_cpus > 1u) {
        dec_threads = (int)vp->logical_cpus - 1;
        if (dec_threads < 2) dec_threads = 2;
        if (dec_threads > 6) dec_threads = 6;
    }
    vp_h264_dec_t* vdec = vp_h264_create(dec_threads);
    if (!vdec) {
        pxe_mutex_lock(&vp->fring.mtx);
        vp->fring.eof = 1;
        pxe_cond_broadcast(&vp->fring.not_empty);
        pxe_mutex_unlock(&vp->fring.mtx);
        return NULL;
    }
    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "[vplayer] OpenH264 decoder active: threads=%d (online_cpus=%u)",
                 dec_threads, (unsigned)vp->logical_cpus);
        pxe_debug_log(dbg);
    }

    size_t annexb_cap = 512u * 1024u;
    uint8_t* annexb = (uint8_t*)malloc(annexb_cap);
    uint32_t cur_serial = vp->seek_serial;

    while (!vp->stop_threads) {
        if (vp->seek_serial != cur_serial) {
            cur_serial = vp->seek_serial;
            vp_h264_reset(vdec);
        }

        vp_vpkt_t pkt;
        memset(&pkt, 0, sizeof(pkt));

        pxe_mutex_lock(&vp->vpq.mtx);
        while (!vp->stop_threads && vp->seek_serial == cur_serial &&
               vp->vpq.count == 0 && !vp->vpq.eof) {
            pxe_cond_timedwait(&vp->vpq.not_empty, &vp->vpq.mtx, 15000000ULL);
        }

        if (vp->stop_threads || vp->seek_serial != cur_serial) {
            pxe_mutex_unlock(&vp->vpq.mtx);
            continue;
        }

        if (vp->vpq.count == 0 && vp->vpq.eof) {
            pxe_mutex_unlock(&vp->vpq.mtx);
            /* Flush any remaining reordered frames */
            for (int fl = 0; fl < 16 && !vp->stop_threads && vp->seek_serial == cur_serial; fl++) {
                vp_h264_frame_t f;
                if (vp_h264_flush(vdec, &f) <= 0 || !f.got_frame) break;
                pxe_mutex_lock(&vp->fring.mtx);
                while (!vp->stop_threads && vp->seek_serial == cur_serial &&
                       vp->fring.count >= vp->fring.cap) {
                    pxe_cond_timedwait(&vp->fring.not_full, &vp->fring.mtx, 15000000ULL);
                }
                if (!vp->stop_threads && vp->seek_serial == cur_serial) {
                    if (yuv_frame_copy(&vp->fring.slots[vp->fring.tail], &f) == 0) {
                        vp->fring.tail = (vp->fring.tail + 1) % vp->fring.cap;
                        vp->fring.count++;
                        pxe_cond_signal(&vp->fring.not_empty);
                    }
                }
                pxe_mutex_unlock(&vp->fring.mtx);
            }
            pxe_mutex_lock(&vp->fring.mtx);
            vp->fring.eof = 1;
            pxe_cond_broadcast(&vp->fring.not_empty);
            pxe_mutex_unlock(&vp->fring.mtx);
            while (!vp->stop_threads && vp->seek_serial == cur_serial) {
                pxe_sleep_ms(10);
            }
            continue;
        }

        pkt = vp->vpq.items[vp->vpq.head];
        vp->vpq.items[vp->vpq.head].data = NULL;
        vp->vpq.head = (vp->vpq.head + 1) % VP_VPQ_CAP;
        vp->vpq.count--;
        pxe_cond_signal(&vp->vpq.not_full);
        pxe_mutex_unlock(&vp->vpq.mtx);

        if (!pkt.data) continue;

        size_t need = (size_t)pkt.size + vp->index.extradata_len + 128u;
        if (need > annexb_cap) {
            uint8_t* nb = (uint8_t*)realloc(annexb, need);
            if (!nb) { free(pkt.data); continue; }
            annexb = nb;
            annexb_cap = need;
        }

        size_t ab_len = vp_avcc_to_annexb(&vp->index, pkt.data, pkt.size,
                                          annexb, annexb_cap, pkt.prepend_sps);
        free(pkt.data);
        if (ab_len == 0) continue;

        vp_h264_frame_t frame;
        if (vp_h264_decode(vdec, annexb, ab_len, pkt.pts_us, &frame) == 0 && frame.got_frame) {
            pxe_mutex_lock(&vp->fring.mtx);
            while (!vp->stop_threads && vp->seek_serial == cur_serial &&
                   vp->fring.count >= vp->fring.cap) {
                pxe_cond_timedwait(&vp->fring.not_full, &vp->fring.mtx, 15000000ULL);
            }
            if (!vp->stop_threads && vp->seek_serial == cur_serial) {
                if (yuv_frame_copy(&vp->fring.slots[vp->fring.tail], &frame) == 0) {
                    vp->fring.tail = (vp->fring.tail + 1) % vp->fring.cap;
                    vp->fring.count++;
                    pxe_cond_signal(&vp->fring.not_empty);
                }
            }
            pxe_mutex_unlock(&vp->fring.mtx);
        }
    }

    if (annexb) free(annexb);
    vp_h264_destroy(vdec);
    return NULL;
}

/* =========================================================================
 * Playback Lifecycle & Seek Control
 * ========================================================================= */

static void vp_stop_pipeline(vplayer_t* vp) {
    if (vp->threads_active) {
        vp->stop_threads = 1;
        vp->buffering = 0;
        pxe_mutex_lock(&vp->sring.mtx);
        pxe_cond_broadcast(&vp->sring.not_empty);
        pxe_cond_broadcast(&vp->sring.not_full);
        pxe_mutex_unlock(&vp->sring.mtx);
        pxe_mutex_lock(&vp->vpq.mtx);
        pxe_cond_broadcast(&vp->vpq.not_empty);
        pxe_cond_broadcast(&vp->vpq.not_full);
        pxe_mutex_unlock(&vp->vpq.mtx);
        pxe_mutex_lock(&vp->fring.mtx);
        pxe_cond_broadcast(&vp->fring.not_empty);
        pxe_cond_broadcast(&vp->fring.not_full);
        pxe_mutex_unlock(&vp->fring.mtx);

        pxe_thread_join(vp->t_io, NULL);
        pxe_thread_join(vp->t_demux, NULL);
        pxe_thread_join(vp->t_vdec, NULL);
        vp->threads_active = 0;
    }

    if (vp->audio_stream_opened) {
        pxe_audio_stop();
        vp->audio_stream_opened = 0;
    }

    vpq_flush(&vp->vpq);
    fring_flush(&vp->fring);
    fring_free_slots(&vp->fring);
    sring_destroy(&vp->sring);

    vp->playing = 0;
    vp->paused = 0;
    vp->buffering = 0;
    vp->cur_pos_us = 0;
    vp->has_last_frame = 0;
}

static void vp_detect_media_drive(vplayer_t* vp, const char* path) {
    vp->is_cdrom = 0;
    vp->is_usb = 0;
    if (!path || !path[0] || path[1] != ':') return;
    char letter = path[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');

    pxe_drive_info_t drives[26];
    int n = pxe_get_drives(drives, 26);
    for (int i = 0; i < n; i++) {
        char dl = drives[i].letter;
        if (dl >= 'a' && dl <= 'z') dl = (char)(dl - 'a' + 'A');
        if (dl == letter) {
            if (drives[i].drv_type == 2) vp->is_cdrom = 1;
            else if (drives[i].drv_type == 3) vp->is_usb = 1;
            break;
        }
    }
}

static void vp_set_error_modal(vplayer_t* vp, const char* title, const char* m1, const char* m2) {
    vp->err_modal = 1;
    strncpy(vp->err_title, title, sizeof(vp->err_title) - 1);
    vp->err_title[sizeof(vp->err_title) - 1] = '\0';
    strncpy(vp->err_msg1, m1, sizeof(vp->err_msg1) - 1);
    vp->err_msg1[sizeof(vp->err_msg1) - 1] = '\0';
    strncpy(vp->err_msg2, m2, sizeof(vp->err_msg2) - 1);
    vp->err_msg2[sizeof(vp->err_msg2) - 1] = '\0';
    vp->ui_dirty = 1;
}

static void vp_extract_basename(const char* path, char* out, size_t cap) {
    const char* p = path;
    const char* last = path;
    while (*p) {
        if (*p == '\\' || *p == '/') last = p + 1;
        p++;
    }
    strncpy(out, last, cap - 1);
    out[cap - 1] = '\0';
}

static void vp_open_file(vplayer_t* vp, const char* path) {
    if (!path || !path[0]) return;
    vp_stop_pipeline(vp);
    vp_media_index_free(&vp->index);
    vp->loaded = 0;
    vp->err_modal = 0;

    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    if (pxe_sys_get_basic_info(&info) == 0) {
        vp->free_ram_mb = info.free_ram_mb;
        vp->logical_cpus = info.logical_cpus ? (uint32_t)info.logical_cpus : 1u;
        vp->has_audio_hw = info.has_audio_hw;
        vp->zh = (info.ui_lang != PXE_LANG_EN);
        vp->volume = info.volume;
        vp->muted = info.audio_muted;
    }

    vp_detect_media_drive(vp, path);

    int rc = vp_media_index_open(path, &vp->index);
    if (rc == VP_OPEN_UNSUPPORTED_SPEC) {
        vp_set_error_modal(vp,
            VP_TR(vp, "暂不支持该视频规格", "Unsupported Video Format"),
            VP_TR(vp, "暂不支持该视频规格（当前最高支持 1080p 60fps H.264）",
                      "Unsupported video format (max supported: 1080p 60fps H.264)"),
            vp->index.reject_reason[0] ? vp->index.reject_reason : "");
        return;
    }
    if (rc != VP_OPEN_OK) {
        vp_set_error_modal(vp,
            VP_TR(vp, "无法打开视频文件", "Cannot Open Video"),
            VP_TR(vp, "文件不存在或不是有效的 MP4/MOV/H.264 视频。",
                      "File not found or not a valid MP4/MOV/H.264 video."),
            path);
        return;
    }

    strncpy(vp->file_path, path, sizeof(vp->file_path) - 1);
    vp->file_path[sizeof(vp->file_path) - 1] = '\0';
    vp_extract_basename(path, vp->file_name, sizeof(vp->file_name));
    vp->loaded = 1;

    /* Adaptive Stream Ring & Decoded Frame Ring sizing (Section 5.3.4) */
    size_t ring_mb = vp->is_cdrom ? 16u : 8u;
    int fring_slots = 8;
    if (vp->free_ram_mb > 0 && vp->free_ram_mb < 160u) {
        ring_mb = 4u;
        fring_slots = 3;
    } else if (vp->free_ram_mb >= 512u && vp->is_cdrom) {
        ring_mb = 24u;
    }

    vp->ring_target_bytes = ring_mb * 1024u * 1024u;
    /* CD pre-buffer: ~3-4 MiB (2-4s) or 80% of file if small */
    if (vp->is_cdrom) {
        size_t pre = 4u * 1024u * 1024u;
        if (vp->index.file_size < pre) pre = (size_t)(vp->index.file_size / 2u);
        if (pre > vp->ring_target_bytes / 2u) pre = vp->ring_target_bytes / 2u;
        vp->prebuf_target_bytes = pre;
        vp->buffering = (pre > 0) ? 1 : 0;
    } else {
        vp->prebuf_target_bytes = 512u * 1024u;
        vp->buffering = 0;
    }

    if (sring_init(&vp->sring, vp->ring_target_bytes) != 0) {
        /* Fallback to 4 MiB */
        vp->ring_target_bytes = 4u * 1024u * 1024u;
        if (sring_init(&vp->sring, vp->ring_target_bytes) != 0) {
            vp_media_index_free(&vp->index);
            vp->loaded = 0;
            vp_set_error_modal(vp,
                VP_TR(vp, "内存不足", "Out of Memory"),
                VP_TR(vp, "无法分配视频流缓存。", "Failed to allocate stream ring buffer."),
                "");
            return;
        }
    }

    vpq_init(&vp->vpq);
    fring_init(&vp->fring, fring_slots);

    /* Open low-latency kernel PCM audio stream if audio track present */
    if (vp->index.has_audio && vp->has_audio_hw) {
        int rate = vp->index.audio_rate > 0 ? vp->index.audio_rate : 44100;
        if (pxe_audio_stream_open(rate, 2, 16, 1) >= 0) {
            vp->audio_stream_opened = 1;
        }
    }

    vp->stop_threads = 0;
    vp->seek_serial = 1;
    vp->seek_target_us = 0;
    vp->seek_start_sample = 0;
    vp->clock_base_us = 0;
    vp->wall_start_ns = pxe_get_time_ns();
    vp->aud_last_played_us = 0;
    vp->aud_last_sample_ns = vp->wall_start_ns;
    vp->playing = 1;
    vp->paused = 0;
    vp->cur_pos_us = 0;

    if (pxe_thread_create_ex(&vp->t_io, io_thread_fn, vp, 128u * 1024u, NULL) == 0 &&
        pxe_thread_create_ex(&vp->t_demux, demux_audio_thread_fn, vp, 256u * 1024u, NULL) == 0 &&
        pxe_thread_create_ex(&vp->t_vdec, video_decode_thread_fn, vp, 512u * 1024u, NULL) == 0) {
        vp->threads_active = 1;
        char dbg[160];
        snprintf(dbg, sizeof(dbg),
                 "[vplayer] SMP pipeline started (cpus=%u): io=%llu demux=%llu vdec=%llu",
                 (unsigned)vp->logical_cpus,
                 (unsigned long long)vp->t_io,
                 (unsigned long long)vp->t_demux,
                 (unsigned long long)vp->t_vdec);
        pxe_debug_log(dbg);
    } else {
        vp_stop_pipeline(vp);
        vp_set_error_modal(vp,
            VP_TR(vp, "线程创建失败", "Thread Error"),
            VP_TR(vp, "无法启动多核解码流水线。", "Failed to start SMP decode pipeline."),
            "");
        return;
    }

    char title[64];
    snprintf(title, sizeof(title), "NekoVideo - %.34s", vp->file_name);
    pxe_win_set_title(vp->win_id, title);
    vp->ui_dirty = 1;
}

static void vp_seek_to(vplayer_t* vp, uint64_t target_us) {
    if (!vp->loaded || !vp->threads_active) return;
    if (target_us > vp->index.duration_us) target_us = vp->index.duration_us;

    uint32_t sidx = vp_media_find_seek_sample(&vp->index, target_us);
    uint64_t actual_us = vp->index.samples[sidx].pts_us;

    vp->seek_target_us = actual_us;
    vp->seek_start_sample = sidx;
    vp->seek_serial++;

    if (vp->audio_stream_opened) {
        pxe_audio_stream_flush();
    }
    sring_flush(&vp->sring);
    vpq_flush(&vp->vpq);
    fring_flush(&vp->fring);

    vp->clock_base_us = actual_us;
    vp->wall_start_ns = pxe_get_time_ns();
    vp->aud_last_played_us = 0;
    vp->aud_last_sample_ns = vp->wall_start_ns;
    vp->cur_pos_us = actual_us;
    vp->pause_pos_us = actual_us;
    if (vp->is_cdrom) vp->buffering = 1;
    vp->ui_dirty = 1;
}

static void vp_toggle_pause(vplayer_t* vp) {
    if (!vp->loaded || !vp->playing) return;
    if (!vp->paused) {
        vp->paused = 1;
        vp->pause_pos_us = vp->cur_pos_us;
        if (vp->audio_stream_opened) pxe_audio_pause();
    } else {
        vp->paused = 0;
        vp->clock_base_us = vp->pause_pos_us;
        vp->wall_start_ns = pxe_get_time_ns();
        if (vp->audio_stream_opened) pxe_audio_resume();
    }
    vp->ui_dirty = 1;
}

/* =========================================================================
 * Audio Master Clock Query (Microsecond Precision)
 * ========================================================================= */

static uint64_t vp_get_master_clock_us(vplayer_t* vp) {
    if (vp->paused) return vp->pause_pos_us;
    uint64_t now_ns = pxe_get_time_ns();
    if (vp->audio_stream_opened) {
        pxe_audio_timing_t at;
        if (pxe_audio_get_timing(&at) == 0 && at.state == 1) {
            if (at.played_us != vp->aud_last_played_us || vp->aud_last_sample_ns == 0) {
                vp->aud_last_played_us = at.played_us;
                vp->aud_last_sample_ns = now_ns;
            }
            /* Smoothly interpolate between HDA DMA position updates (clamped to <= 35ms) */
            uint64_t interp_us = (now_ns > vp->aud_last_sample_ns)
                                     ? ((now_ns - vp->aud_last_sample_ns) / 1000ULL)
                                     : 0ULL;
            if (interp_us > 35000ULL) interp_us = 35000ULL;
            return vp->clock_base_us + at.played_us + interp_us;
        }
    }
    uint64_t elap_us = (now_ns > vp->wall_start_ns) ? ((now_ns - vp->wall_start_ns) / 1000ULL) : 0ULL;
    return vp->clock_base_us + elap_us;
}

/* =========================================================================
 * Rendering (Video Area + Classic Transport Bar + Bilingual Overlays)
 * ========================================================================= */

static void fmt_time(uint64_t us, char* out, size_t cap) {
    uint64_t sec = us / 1000000ULL;
    uint32_t m = (uint32_t)(sec / 60ULL);
    uint32_t s = (uint32_t)(sec % 60ULL);
    snprintf(out, cap, "%02u:%02u", m, s);
}

static void vp_compute_video_rect(const vplayer_t* vp, int vw, int vh,
                                  int* ox, int* oy, int* ow, int* oh) {
    int area_w = vp->surf.width;
    int area_h = vp->surf.height - VP_CTRL_BAR_H;
    if (area_h < 32) area_h = 32;
    if (vw <= 0 || vh <= 0) {
        *ox = 0; *oy = 0; *ow = area_w; *oh = area_h;
        return;
    }
    int64_t lhs = (int64_t)area_w * (int64_t)vh;
    int64_t rhs = (int64_t)area_h * (int64_t)vw;
    if (lhs <= rhs) {
        *ow = area_w;
        *oh = (int)((int64_t)area_w * (int64_t)vh / (int64_t)vw);
    } else {
        *oh = area_h;
        *ow = (int)((int64_t)area_h * (int64_t)vw / (int64_t)vh);
    }
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
    *ox = (area_w - *ow) / 2;
    *oy = (area_h - *oh) / 2;
}

static void vp_draw_frame_and_ui(vplayer_t* vp, const vp_yuv_frame_t* frame) {
    pxe_win_surface_t* s = &vp->surf;
    if (!s->pixels || s->width <= 0 || s->height <= 0) return;

    int area_w = s->width;
    int area_h = s->height - VP_CTRL_BAR_H;
    if (area_h < 0) area_h = 0;

    /* 1. Video Canvas */
    if (frame && frame->y && frame->w > 0 && frame->h > 0 && area_h > 0) {
        int vx, vy, vw, vh;
        vp_compute_video_rect(vp, frame->w, frame->h, &vx, &vy, &vw, &vh);
        /* Fill letterbox/pillarbox borders only */
        if (vy > 0) {
            pxe_fill_rect(s, 0, 0, area_w, vy, 0x000000);
            pxe_fill_rect(s, 0, vy + vh, area_w, area_h - (vy + vh), 0x000000);
        }
        if (vx > 0) {
            pxe_fill_rect(s, 0, vy, vx, vh, 0x000000);
            pxe_fill_rect(s, vx + vw, vy, area_w - (vx + vw), vh, 0x000000);
        }
        vp_yuv420_to_surface(s, vx, vy, vw, vh,
                             frame->y, frame->y_stride,
                             frame->u, frame->v, frame->uv_stride,
                             frame->w, frame->h,
                             vp->index.bt709);
    } else if (area_h > 0) {
        pxe_fill_rect(s, 0, 0, area_w, area_h, 0x10141A);
        const char* hint1 = VP_TR(vp, "Pixel Neko 视频播放器 (最高 1080p 60fps H.264)",
                                      "Pixel Neko Video Player (Up to 1080p 60fps H.264)");
        const char* hint2 = VP_TR(vp, "按 O 或点击“打开”加载 MP4 / MOV / H.264 视频",
                                      "Press O or click 'Open' to load MP4 / MOV / H.264");
        int w1 = pxe_text_width(hint1);
        int w2 = pxe_text_width(hint2);
        pxe_draw_text(s, (area_w - w1) / 2, area_h / 2 - 18, hint1, 0xE8F0FF);
        pxe_draw_text(s, (area_w - w2) / 2, area_h / 2 + 4, hint2, 0x8CA0B8);
    }

    /* Buffering overlay (especially helpful on CD-ROM pre-buffering) */
    if (vp->buffering && area_h > 60) {
        int bw = 260, bh = 54;
        int bx = (area_w - bw) / 2;
        int by = (area_h - bh) / 2;
        pxe_draw_bevel_box(s, bx, by, bw, bh, 0, 0xDCDCDC);
        int pct = 0;
        if (vp->prebuf_target_bytes > 0) {
            pct = (int)((vp->sring.used * 100u) / vp->prebuf_target_bytes);
            if (pct > 100) pct = 100;
        }
        char btxt[64];
        snprintf(btxt, sizeof(btxt), "%s %d%%",
                 VP_TR(vp, "正在缓冲...", "Buffering..."), pct);
        pxe_draw_text(s, bx + 14, by + 10, btxt, 0x101010);
        pxe_draw_bevel_box(s, bx + 14, by + 32, bw - 28, 12, 1, 0xFFFFFF);
        int fill = ((bw - 32) * pct) / 100;
        if (fill > 0) pxe_fill_rect(s, bx + 16, by + 34, fill, 8, 0x2060C0);
    }

    /* 2. Bottom Transport Bar */
    int bar_y = area_h;
    pxe_fill_rect(s, 0, bar_y, area_w, VP_CTRL_BAR_H, 0xD4D0C8);
    pxe_draw_hline(s, 0, bar_y, area_w, 0xFFFFFF);

    /* Seek slider */
    int sx = 10, sy = bar_y + 6, sw = area_w - 20, sh = 10;
    if (sw > 40) {
        pxe_draw_bevel_box(s, sx, sy, sw, sh, 1, 0x20242A);
        if (vp->loaded && vp->index.duration_us > 0) {
            uint64_t pos = vp->cur_pos_us;
            if (pos > vp->index.duration_us) pos = vp->index.duration_us;
            int fill = (int)((pos * (uint64_t)(sw - 4)) / vp->index.duration_us);
            if (fill > 0) pxe_fill_rect(s, sx + 2, sy + 2, fill, sh - 4, 0x2A78E4);
            int kx = sx + 2 + fill - 3;
            if (kx < sx) kx = sx;
            if (kx > sx + sw - 6) kx = sx + sw - 6;
            pxe_draw_bevel_box(s, kx, sy - 2, 7, sh + 4, 0, 0xECE9D8);
        }
    }

    /* Transport buttons */
    int by = bar_y + 21;
    pxe_draw_button(s, 10,  by, 56, 22, vp->paused ? VP_TR(vp, "播放", "Play") : VP_TR(vp, "暂停", "Pause"), 0, vp->loaded);
    pxe_draw_button(s, 72,  by, 52, 22, VP_TR(vp, "停止", "Stop"), 0, vp->loaded);
    pxe_draw_button(s, 130, by, 56, 22, VP_TR(vp, "打开", "Open"), 0, 1);
    pxe_draw_button(s, 192, by, 56, 22, vp->muted ? VP_TR(vp, "取消静音", "Unmute") : VP_TR(vp, "静音", "Mute"), 0, 1);

    /* Time & spec badge */
    char t_cur[16], t_tot[16], info_line[96];
    fmt_time(vp->cur_pos_us, t_cur, sizeof(t_cur));
    fmt_time(vp->loaded ? vp->index.duration_us : 0, t_tot, sizeof(t_tot));
    if (vp->loaded) {
        snprintf(info_line, sizeof(info_line), "%s / %s  %dx%d@%dfps%s",
                 t_cur, t_tot, vp->index.width, vp->index.height,
                 (vp->index.fps_x100 + 50) / 100,
                 vp->is_cdrom ? " [CD]" : "");
    } else {
        snprintf(info_line, sizeof(info_line), "00:00 / 00:00");
    }
    pxe_draw_text(s, 258, by + 3, info_line, 0x101010);

    /* 3. Open Path Modal */
    if (vp->open_modal) {
        int mw = 380, mh = 104;
        if (mw > area_w - 20) mw = area_w - 20;
        int mx = (area_w - mw) / 2;
        int my = (s->height - mh) / 2;
        pxe_draw_bevel_box(s, mx, my, mw, mh, 0, 0xD4D0C8);
        pxe_fill_rect(s, mx + 2, my + 2, mw - 4, 20, 0x0A246A);
        pxe_draw_text(s, mx + 8, my + 4, VP_TR(vp, "打开视频文件 (MP4 / MOV / H264)", "Open Video (MP4 / MOV / H264)"), 0xFFFFFF);
        pxe_draw_bevel_box(s, mx + 10, my + 32, mw - 20, 24, 1, 0xFFFFFF);
        pxe_draw_text(s, mx + 14, my + 36, vp->open_input, 0x000000);
        pxe_draw_button(s, mx + mw - 144, my + 68, 62, 24, VP_TR(vp, "确定", "OK"), 0, 1);
        pxe_draw_button(s, mx + mw - 74,  my + 68, 62, 24, VP_TR(vp, "取消", "Cancel"), 0, 1);
    }

    /* 4. Error / Spec Rejection Modal */
    if (vp->err_modal) {
        int mw = 460, mh = 128;
        if (mw > area_w - 20) mw = area_w - 20;
        int mx = (area_w - mw) / 2;
        int my = (s->height - mh) / 2;
        pxe_draw_bevel_box(s, mx, my, mw, mh, 0, 0xD4D0C8);
        pxe_fill_rect(s, mx + 2, my + 2, mw - 4, 20, 0xA01818);
        pxe_draw_text(s, mx + 8, my + 4, vp->err_title, 0xFFFFFF);
        pxe_draw_text(s, mx + 14, my + 34, vp->err_msg1, 0x101010);
        if (vp->err_msg2[0]) {
            pxe_draw_text(s, mx + 14, my + 58, vp->err_msg2, 0x505050);
        }
        pxe_draw_button(s, mx + (mw - 76) / 2, my + 90, 76, 24, VP_TR(vp, "确定", "OK"), 0, 1);
    }
}

static void vp_present_now(vplayer_t* vp, const vp_yuv_frame_t* frame) {
    vp_draw_frame_and_ui(vp, frame);
    pxe_win_swap_buffer(vp->win_id, &vp->surf);
    pxe_win_present(vp->win_id);
    vp->ui_dirty = 0;
}

/* =========================================================================
 * Event Handling
 * ========================================================================= */

static void vp_setup_menu(vplayer_t* vp) {
    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;

    strncpy(mb.menus[0].title, VP_TR(vp, "文件", "File"), sizeof(mb.menus[0].title) - 1);
    mb.menus[0].item_count = 3;
    strncpy(mb.menus[0].items[0].label, VP_TR(vp, "打开视频...", "Open Video..."), 23); mb.menus[0].items[0].enabled = 1;
    strncpy(mb.menus[0].items[1].label, VP_TR(vp, "关闭视频", "Close Video"), 23);     mb.menus[0].items[1].enabled = 1;
    strncpy(mb.menus[0].items[2].label, VP_TR(vp, "退出", "Exit"), 23);                mb.menus[0].items[2].enabled = 1;

    strncpy(mb.menus[1].title, VP_TR(vp, "播放", "Playback"), sizeof(mb.menus[1].title) - 1);
    mb.menus[1].item_count = 4;
    strncpy(mb.menus[1].items[0].label, VP_TR(vp, "播放 / 暂停", "Play / Pause"), 23); mb.menus[1].items[0].enabled = 1;
    strncpy(mb.menus[1].items[1].label, VP_TR(vp, "停止", "Stop"), 23);                mb.menus[1].items[1].enabled = 1;
    strncpy(mb.menus[1].items[2].label, VP_TR(vp, "快退 10 秒", "Seek -10s"), 23);     mb.menus[1].items[2].enabled = 1;
    strncpy(mb.menus[1].items[3].label, VP_TR(vp, "快进 10 秒", "Seek +10s"), 23);     mb.menus[1].items[3].enabled = 1;

    pxe_win_set_menu(vp->win_id, &mb);
}

static void vp_handle_event(vplayer_t* vp, const pxe_event_t* ev) {
    switch (ev->type) {
        case PXE_EV_CLOSE:
            vp->running = 0;
            break;

        case PXE_EV_RESIZE:
            pxe_win_get_surface(vp->win_id, &vp->surf);
            vp->ui_dirty = 1;
            break;

        case PXE_EV_REDRAW:
            vp->ui_dirty = 1;
            break;

        case PXE_EV_ACTIVATE:
            if (ev->text[0]) {
                vp_open_file(vp, ev->text);
            }
            break;

        case PXE_EV_MENU:
            if (ev->arg0 == 0) {
                if (ev->arg1 == 0) {
                    vp->open_modal = 1;
                    vp->open_input[0] = '\0';
                    vp->open_len = 0;
                    vp->ui_dirty = 1;
                } else if (ev->arg1 == 1) {
                    vp_stop_pipeline(vp);
                    vp_media_index_free(&vp->index);
                    vp->loaded = 0;
                    vp->ui_dirty = 1;
                } else if (ev->arg1 == 2) {
                    vp->running = 0;
                }
            } else if (ev->arg0 == 1) {
                if (ev->arg1 == 0) vp_toggle_pause(vp);
                else if (ev->arg1 == 1) { vp_stop_pipeline(vp); vp->ui_dirty = 1; }
                else if (ev->arg1 == 2 && vp->loaded) {
                    uint64_t t = vp->cur_pos_us > 10000000ULL ? (vp->cur_pos_us - 10000000ULL) : 0ULL;
                    vp_seek_to(vp, t);
                } else if (ev->arg1 == 3 && vp->loaded) {
                    vp_seek_to(vp, vp->cur_pos_us + 10000000ULL);
                }
            }
            break;

        case PXE_EV_KEY_DOWN:
            if (vp->err_modal) {
                if (ev->key == PXE_KEY_ENTER || ev->key == PXE_KEY_ESC || ev->key == ' ') {
                    vp->err_modal = 0;
                    vp->ui_dirty = 1;
                }
                break;
            }
            if (vp->open_modal) {
                if (ev->key == PXE_KEY_ESC) {
                    vp->open_modal = 0;
                    vp->ui_dirty = 1;
                } else if (ev->key == PXE_KEY_ENTER) {
                    vp->open_modal = 0;
                    vp_open_file(vp, vp->open_input);
                } else if (ev->key == PXE_KEY_BACKSPACE) {
                    if (vp->open_len > 0) {
                        vp->open_input[--vp->open_len] = '\0';
                        vp->ui_dirty = 1;
                    }
                } else if (ev->key >= 32 && ev->key < 127 && vp->open_len < 250) {
                    vp->open_input[vp->open_len++] = (char)ev->key;
                    vp->open_input[vp->open_len] = '\0';
                    vp->ui_dirty = 1;
                }
                break;
            }
            if (ev->key == ' ') {
                vp_toggle_pause(vp);
            } else if (ev->key == 'o' || ev->key == 'O') {
                vp->open_modal = 1;
                vp->open_input[0] = '\0';
                vp->open_len = 0;
                vp->ui_dirty = 1;
            } else if (ev->key == 'm' || ev->key == 'M') {
                pxe_audio_toggle_mute();
                vp->muted = !vp->muted;
                vp->ui_dirty = 1;
            } else if (ev->key == PXE_KEY_LEFT && vp->loaded) {
                uint64_t t = vp->cur_pos_us > 5000000ULL ? (vp->cur_pos_us - 5000000ULL) : 0ULL;
                vp_seek_to(vp, t);
            } else if (ev->key == PXE_KEY_RIGHT && vp->loaded) {
                vp_seek_to(vp, vp->cur_pos_us + 5000000ULL);
            }
            break;

        case PXE_EV_MOUSE_DOWN: {
            int x = ev->x, y = ev->y;
            int area_w = vp->surf.width;
            int area_h = vp->surf.height - VP_CTRL_BAR_H;
            if (vp->err_modal) {
                vp->err_modal = 0;
                vp->ui_dirty = 1;
                break;
            }
            if (vp->open_modal) {
                int mw = 380, mh = 104;
                if (mw > area_w - 20) mw = area_w - 20;
                int mx = (area_w - mw) / 2;
                int my = (vp->surf.height - mh) / 2;
                if (pxe_point_in_rect(x, y, mx + mw - 144, my + 68, 62, 24)) {
                    vp->open_modal = 0;
                    vp_open_file(vp, vp->open_input);
                } else if (pxe_point_in_rect(x, y, mx + mw - 74, my + 68, 62, 24)) {
                    vp->open_modal = 0;
                    vp->ui_dirty = 1;
                }
                break;
            }
            /* Seek bar click */
            int sx = 10, sy = area_h + 4, sw = area_w - 20, sh = 14;
            if (vp->loaded && vp->index.duration_us > 0 && pxe_point_in_rect(x, y, sx, sy, sw, sh)) {
                uint64_t tgt = ((uint64_t)(x - sx) * vp->index.duration_us) / (uint64_t)sw;
                vp_seek_to(vp, tgt);
                break;
            }
            int by = area_h + 21;
            if (pxe_point_in_rect(x, y, 10, by, 56, 22)) {
                vp_toggle_pause(vp);
            } else if (pxe_point_in_rect(x, y, 72, by, 52, 22)) {
                vp_stop_pipeline(vp);
                vp->ui_dirty = 1;
            } else if (pxe_point_in_rect(x, y, 130, by, 56, 22)) {
                vp->open_modal = 1;
                vp->open_input[0] = '\0';
                vp->open_len = 0;
                vp->ui_dirty = 1;
            } else if (pxe_point_in_rect(x, y, 192, by, 56, 22)) {
                pxe_audio_toggle_mute();
                vp->muted = !vp->muted;
                vp->ui_dirty = 1;
            }
            break;
        }
        default:
            break;
    }
}

/* =========================================================================
 * Main Entry Point (Thread 4: Presenter & UI Event Loop)
 * ========================================================================= */

int main(int argc, char** argv) {
    vplayer_t* vp = (vplayer_t*)calloc(1, sizeof(vplayer_t));
    if (!vp) pxe_exit(-1);

    vp->zh = pxe_is_zh();
    vp->running = 1;

    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    int init_w = 640, init_h = 408;
    if (pxe_sys_get_basic_info(&info) == 0) {
        vp->free_ram_mb = info.free_ram_mb;
        vp->has_audio_hw = info.has_audio_hw;
        vp->volume = info.volume;
        vp->muted = info.audio_muted;
        if (info.screen_w >= 1024 && info.screen_h >= 720) {
            init_w = 854;
            init_h = 480 + VP_CTRL_BAR_H;
        }
    }

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x      = -1;
    desc.y      = -1;
    desc.w      = init_w;
    desc.h      = init_h;
    desc.min_w  = 420;
    desc.min_h  = 280;
    desc.format = PXE_CANVAS_RGB32;
    desc.flags  = PXE_WIN_RESIZABLE | PXE_WIN_CENTERED | PXE_WIN_TRIPLE_BUF;
    strncpy(desc.title, vp->zh ? "NekoVideo 视频播放器" : "NekoVideo Player", sizeof(desc.title) - 1);

    vp->win_id = pxe_win_create(&desc, &vp->surf);
    if (vp->win_id < 0) {
        free(vp);
        pxe_exit(-1);
    }

    vp_setup_menu(vp);
    vp->ui_dirty = 1;

    if (argc >= 2 && argv[1] && argv[1][0]) {
        vp_open_file(vp, argv[1]);
    }

    vp_present_now(vp, NULL);

    uint64_t last_ui_ns = pxe_get_time_ns();

    while (vp->running) {
        pxe_event_t ev;
        while (pxe_win_poll_event(vp->win_id, &ev) > 0) {
            vp_handle_event(vp, &ev);
        }
        if (!vp->running) break;

        if (vp->loaded && vp->playing && !vp->paused && !vp->buffering && vp->threads_active) {
            uint64_t master_us = vp_get_master_clock_us(vp);
            vp->cur_pos_us = master_us;

            int presented = 0;
            pxe_mutex_lock(&vp->fring.mtx);
            while (vp->fring.count > 0) {
                vp_yuv_frame_t* f = &vp->fring.slots[vp->fring.head];
                if (f->pts_us + 65000ULL < master_us && vp->fring.count > 1) {
                    /* Drop late frame to maintain smooth A/V sync */
                    vp->fring.head = (vp->fring.head + 1) % vp->fring.cap;
                    vp->fring.count--;
                    pxe_cond_signal(&vp->fring.not_full);
                    continue;
                }
                if (f->pts_us <= master_us + 3000ULL) {
                    /* O(1) zero-copy buffer swap with last_frame while holding mutex */
                    vp_yuv_frame_t old_last = vp->last_frame;
                    vp->last_frame = *f;
                    *f = old_last;
                    vp->fring.head = (vp->fring.head + 1) % vp->fring.cap;
                    vp->fring.count--;
                    vp->has_last_frame = 1;
                    pxe_cond_signal(&vp->fring.not_full);
                    pxe_mutex_unlock(&vp->fring.mtx);

                    vp_present_now(vp, &vp->last_frame);
                    presented = 1;
                    pxe_mutex_lock(&vp->fring.mtx);
                    break;
                } else {
                    break;
                }
            }
            int at_eof = (vp->fring.count == 0 && vp->fring.eof);
            pxe_mutex_unlock(&vp->fring.mtx);

            if (at_eof) {
                vp->playing = 0;
                vp->cur_pos_us = vp->index.duration_us;
                vp->ui_dirty = 1;
            }
            if (!presented) {
                pxe_sleep_us(1000ULL);
            }
        } else {
            uint64_t now_ns = pxe_get_time_ns();
            if (vp->buffering && now_ns - last_ui_ns >= 80000000ULL) {
                vp->ui_dirty = 1;
                last_ui_ns = now_ns;
            }
            if (vp->ui_dirty) {
                vp_present_now(vp, vp->has_last_frame ? &vp->last_frame : NULL);
            }
            pxe_sleep_ms(12);
        }
    }

    vp_stop_pipeline(vp);
    vp_media_index_free(&vp->index);
    if (vp->last_frame.y) free(vp->last_frame.y);
    pxe_win_destroy(vp->win_id);
    free(vp);
    pxe_exit(0);
    return 0;
}

