#ifndef LIB__FB_H__
#define LIB__FB_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/edid.h>

struct resolution {
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
};

#define FB_SOURCE_NONE 0
#define FB_SOURCE_PCI 1

// Source-probe outcome bits, recorded per framebuffer while the source
// proofs run.  Wire vocabulary: values must match the
// LIMINE_MP_FRAMEBUFFER_PROBE_* constants in protos/limine_mp.h and the
// revision-1 contract in EXTENSIONS.md.
#define FB_PROBE_DP_PRESENT     (1 << 0)  // GOP handle carries a device path
#define FB_PROBE_DP_PCI_PREFIX  (1 << 1)  // that path has an EFI_PCI_IO prefix
#define FB_PROBE_DP_LOCATED     (1 << 2)  // prefix handle yielded PCI coordinates
#define FB_PROBE_CHILD_SCAN     (1 << 3)  // child-controller relation scan ran
#define FB_PROBE_CHILD_RELATION (1 << 4)  // a PCI parent names this GOP handle
#define FB_PROBE_CONSOLE_HANDLE (1 << 5)  // GOP handle == gST->ConsoleOutHandle
#define FB_PROBE_CONOUT_VAR     (1 << 6)  // ConOut variable read and walked
#define FB_PROBE_CONOUT_PCI     (1 << 7)  // >=1 ConOut instance reached PCI I/O
#define FB_PROBE_CONOUT_UNIQUE  (1 << 8)  // every resolved instance agrees on one BDF
#define FB_PROBE_SOURCE_PCI     (1 << 9)  // final verdict: source_type == PCI

// [23:16] GOP handle count seen by init_gop, [31:24] this framebuffer's
// handle index, both saturating at 0xff.
#define FB_PROBE_HANDLES_SHIFT 16
#define FB_PROBE_INDEX_SHIFT 24

struct fb_pci_source {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

struct fb_info {
    uint64_t framebuffer_pitch;
    uint64_t framebuffer_width;
    uint64_t framebuffer_height;
    uint16_t framebuffer_bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;

    uint64_t framebuffer_addr;

    struct edid_info_struct *edid;

    uint64_t mode_count;
    struct fb_info *mode_list;

    uint8_t source_type;
    struct fb_pci_source pci_source;
    uint32_t source_probe;
};

extern struct fb_info *fb_fbs;
extern size_t fb_fbs_count;

void fb_init(struct fb_info **ret, size_t *_fbs_count,
             uint64_t target_width, uint64_t target_height, uint16_t target_bpp,
             bool preserve_screen, bool keep_wc);

void fb_clear(struct fb_info *fb);

bool fb_flush_reliable(void);

// False means no mechanism exists, not that a flush was attempted and failed.
bool fb_flush(volatile void *base, size_t length);

// flanterm's callback type has no return value.
void fb_flush_cb(volatile void *base, size_t length);

#endif
