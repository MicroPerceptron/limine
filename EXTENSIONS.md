# MicroPerceptron Limine extensions

This fork implements optional Limine protocol features used by MicroPerceptron
systems. Unsupported bootloaders leave each extension request's response
untouched, as they do for any other unknown Limine request.

## Framebuffer source

The framebuffer source feature identifies the firmware device that produced
each entry in the standard Limine framebuffer response.

```c
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
    void *data;
};

struct limine_mp_framebuffer_source_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_mp_framebuffer_source **entries;
};

struct limine_mp_framebuffer_source_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_mp_framebuffer_source_response *response;
};
```

The request requires the standard Limine framebuffer request. The response is
omitted when that request is absent or has no framebuffers. `entry_count` equals
the standard response's `framebuffer_count`, and entries correspond by index.

For `LIMINE_MP_FRAMEBUFFER_SOURCE_UNKNOWN`, `data` is `NULL`. For
`LIMINE_MP_FRAMEBUFFER_SOURCE_PCI`, `data` points to a
`limine_mp_framebuffer_pci_source`. Segment, bus, device, and function use PCI
segment-group numbering. Values outside the PCI ranges are never reported as a
PCI source.

UEFI GOP handles are first resolved through their device paths to the closest
handle supporting `EFI_PCI_IO_PROTOCOL`. If firmware installs GOP on a child
whose device path cannot be resolved to PCI I/O, the loader accepts only an
explicit `EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER` relationship naming that GOP
handle. The selected PCI I/O protocol's `GetLocation()` method provides the PCI
coordinates. Failure of both positive relationships, a non-PCI provider, and
BIOS VBE all produce an unknown source rather than an inferred identity.
