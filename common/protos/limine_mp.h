#ifndef PROTOS__LIMINE_MP_H__
#define PROTOS__LIMINE_MP_H__

#include <stdint.h>

#ifndef LIMINE_H
#error "include <limine.h> before <protos/limine_mp.h>"
#endif

#define LIMINE_MP_FRAMEBUFFER_SOURCE_REQUEST_ID \
    { LIMINE_COMMON_MAGIC, 0x28143af3420a1fff, 0xd1f11b7ac1c98b44 }

#define LIMINE_MP_FRAMEBUFFER_SOURCE_UNKNOWN 0
#define LIMINE_MP_FRAMEBUFFER_SOURCE_PCI 1

// Revision-1 probe-mask bits: which source proofs ran and how far each got,
// per framebuffer.  Values mirror the FB_PROBE_* constants in common/lib/fb.h
// (the loader-side recorder); EXTENSIONS.md is the contract.
#define LIMINE_MP_FRAMEBUFFER_PROBE_DP_PRESENT     (1 << 0)
#define LIMINE_MP_FRAMEBUFFER_PROBE_DP_PCI_PREFIX  (1 << 1)
#define LIMINE_MP_FRAMEBUFFER_PROBE_DP_LOCATED     (1 << 2)
#define LIMINE_MP_FRAMEBUFFER_PROBE_CHILD_SCAN     (1 << 3)
#define LIMINE_MP_FRAMEBUFFER_PROBE_CHILD_RELATION (1 << 4)
#define LIMINE_MP_FRAMEBUFFER_PROBE_CONSOLE_HANDLE (1 << 5)
#define LIMINE_MP_FRAMEBUFFER_PROBE_CONOUT_VAR     (1 << 6)
#define LIMINE_MP_FRAMEBUFFER_PROBE_CONOUT_PCI     (1 << 7)
#define LIMINE_MP_FRAMEBUFFER_PROBE_CONOUT_UNIQUE  (1 << 8)
#define LIMINE_MP_FRAMEBUFFER_PROBE_SOURCE_PCI     (1 << 9)
#define LIMINE_MP_FRAMEBUFFER_PROBE_HANDLES_SHIFT  16
#define LIMINE_MP_FRAMEBUFFER_PROBE_INDEX_SHIFT    24

struct limine_mp_framebuffer_pci_source {
    uint64_t segment;
    uint64_t bus;
    uint64_t device;
    uint64_t function;
};

struct limine_mp_framebuffer_source {
    uint64_t type;
    LIMINE_PTR(void *) data;
};

struct limine_mp_framebuffer_source_response {
    uint64_t revision;
    uint64_t entry_count;
    LIMINE_PTR(struct limine_mp_framebuffer_source **) entries;
    // Revision >= 1: `entry_count` probe masks, index-aligned with `entries`.
    // Readers must gate on `revision` before touching this field.
    LIMINE_PTR(uint64_t *) probe_masks;
};

struct limine_mp_framebuffer_source_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_mp_framebuffer_source_response *) response;
};

#endif
