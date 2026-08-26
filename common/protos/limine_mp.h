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
};

struct limine_mp_framebuffer_source_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_mp_framebuffer_source_response *) response;
};

#endif
