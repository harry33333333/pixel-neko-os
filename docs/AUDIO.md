# Pixel Neko Audio Subsystem

## Overview

Pixel Neko OS provides a unified 64-bit audio engine, real-time DSP pipeline, and streaming scheduler. It transparently abstracts multiple hardware audio controllers behind a single driver interface and supports uncompressed (WAV), lossless (FLAC), and lossy (MP3) formats.

## Hardware Drivers

The kernel probes for audio hardware during PCI enumeration and binds the first functional controller:

| Driver | Controller | Features |
| --- | --- | --- |
| `hdaudio` | Intel High Definition Audio (Azalia) | 64-bit BDL DMA, CORB/RIRB codec verbs, 48 kHz 16-bit stereo |
| `ac97` | AC'97 2.3 Audio Controller | Bus Master DMA, 32-entry buffer descriptor list |
| `cmi8738` | C-Media CMI8738 PCI Sound Card | Hardware bus mastering, dual-channel DAC |
| `ct4810` | Creative Sound Blaster PCI (ES1371/CT4810) | Legacy Sound Blaster compatibility, frame-synced playback |
| `speaker` | PC Speaker (PIT Counter 2 + Port 0x61) | Square-wave tones and boot chimes fallback |

## Decoders & Formats

- **WAV (PCM)**: Supports 8-bit, 16-bit, and 24-bit PCM at mono or stereo configurations. Automatic integer downsampling/decimation (2x, 4x) to match hardware output rates.
- **FLAC**: Multi-channel lossless decoding with dynamic bit-depth expansion.
- **MP3**: Powered by `minimp3`:
  - Compiled with `-msse -msse2 -fno-tree-vectorize -mstackrealign` to enforce 16-byte stack and structure alignment.
  - Stream buffer sized to 32 KiB (`s_mp3_buf`) to prevent starvation during complex high-bitrate VBR frames.
  - Fully eliminates General Protection Faults (`#GP`) on modern CPUs during vector arithmetic.

## Anti-Stuttering & Ring Buffer Architecture

To eliminate playback stuttering and audio glitches under high CPU or UI load:

1. **Large Queue Capacity**:
   - `PCM_QUEUE_SIZE` is set to 262,144 bytes (256 KiB, ~65,536 16-bit stereo samples), offering up to ~1.5 seconds of PCM cushion.
2. **Chunked Memory Transfers**:
   - Queue indexing uses power-of-two bitmasks `(PCM_QUEUE_SAMPLES - 1)` with vectorized `memcpy` operations rather than per-sample modulo increments, drastically reducing CPU overhead.
3. **Startup Pre-Buffering**:
   - `audio_play_file()` pre-decodes up to 2 full DMA half-buffers (64 KiB) into the queue before activating hardware DMA (`start_dma()`), preventing startup underflow.
4. **Adaptive Decoding Budget**:
   - During `audio_poll()`, when the queue falls below 2 DMA halves, the decoder bursts up to 24 frames; otherwise it processes 12 frames per tick to balance CPU consumption.
5. **Multi-Point Scheduler Polling**:
   - In addition to standard 20 Hz / 100 Hz timer ticks, `audio_poll()` is hooked into the kernel scheduler idle loop (`scheduler_idle_entry`) and the window manager backbuffer flip routine (`ui_compat_flip`), guaranteeing low-latency DMA replenishment even during intensive UI repaints.

## Visualizer & DSP

The audio engine computes real-time 128-band frequency spectrum and stereo peak levels directly from the active hardware DMA buffer. The visualizer feeds the vintage Mac OS 9-style media player application without adding extra decoding passes.
