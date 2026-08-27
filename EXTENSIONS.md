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
    // Revision >= 1 only.
    uint64_t *probe_masks;
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
handle. When both relations are absent and the GOP handle is exactly the
system table's `ConsoleOutHandle` — the console-splitter posture, where the
aggregate handle carries no device path and no recorded PCI parent — the
loader resolves the firmware's own `ConOut` variable: every instance of that
multi-instance device path that reaches an `EFI_PCI_IO_PROTOCOL` handle must
agree on one PCI function, which is then the console framebuffer's source.
The selected PCI I/O protocol's `GetLocation()` method provides the PCI
coordinates in every proof. Failure of all three positive relationships, a
non-PCI provider, a `ConOut` naming two PCI devices, and BIOS VBE all produce
an unknown source rather than an inferred identity.

### Revision 1: probe masks

A revision >= 1 response adds `probe_masks`: `entry_count` `uint64_t` values,
index-aligned with `entries`, recording how far each source proof advanced
for that framebuffer. A serial-less machine can then name the exact firmware
shape that defeated attribution from the mask alone.

```c
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
```

Bits `[23:16]` carry the number of GOP handles the loader enumerated and bits
`[31:24]` this framebuffer's handle index, both saturating at `0xff`; BIOS VBE
framebuffers report a zero mask. Bit meanings are fixed for revision 1;
future proof steps append bits rather than renumbering.
