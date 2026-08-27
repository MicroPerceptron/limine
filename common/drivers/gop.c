#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <efi.h>
#include <lib/misc.h>
#include <lib/term.h>
#include <drivers/gop.h>
#include <drivers/edid.h>
#include <lib/print.h>
#include <mm/pmm.h>

static uint16_t linear_masks_to_bpp(uint32_t red_mask, uint32_t green_mask,
                                    uint32_t blue_mask, uint32_t alpha_mask) {
    uint32_t compound_mask = red_mask | green_mask | blue_mask | alpha_mask;
    uint16_t ret = 32;
    while ((compound_mask & (1U << 31)) == 0) {
        ret--;
        compound_mask <<= 1;
    }
    // Round up to whole bytes: a 5/5/5 mask occupies 2 bytes, not 15 bits.
    return (ret + 7) & ~7;
}

static void linear_mask_to_mask_shift(
                uint8_t *mask, uint8_t *shift, uint32_t linear_mask) {
    *shift = 0;
    *mask = 0;
    if (linear_mask == 0) {
        return;
    }
    while ((linear_mask & 1) == 0) {
        (*shift)++;
        linear_mask >>= 1;
    }
    while ((linear_mask & 1) == 1) {
        (*mask)++;
        linear_mask >>= 1;
    }
}

static bool validate_pitch(struct fb_info *ret, size_t mode) {
    uint64_t bytes_per_pixel = ret->framebuffer_bpp / 8;
    if (bytes_per_pixel == 0
     || ret->framebuffer_pitch % bytes_per_pixel != 0
     || ret->framebuffer_pitch < ret->framebuffer_width * bytes_per_pixel) {
        printv("gop: Mode %u has invalid pitch %u (width=%u, bpp=%u), skipping.\n",
               (uint32_t)mode, (uint32_t)ret->framebuffer_pitch,
               (uint32_t)ret->framebuffer_width, (uint32_t)ret->framebuffer_bpp);
        return false;
    }
    return true;
}

// Most of this code taken from https://wiki.osdev.org/GOP

static bool mode_to_fb_info(struct fb_info *ret, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, size_t mode) {
    EFI_STATUS status;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
    UINTN mode_info_size;

    status = gop->QueryMode(gop, mode, &mode_info_size, &mode_info);

    if (status) {
        return false;
    }

    bool ok = false;

    switch (mode_info->PixelFormat) {
        case PixelBlueGreenRedReserved8BitPerColor:
            ret->framebuffer_bpp = 32;
            ret->red_mask_size = 8;
            ret->red_mask_shift = 16;
            ret->green_mask_size = 8;
            ret->green_mask_shift = 8;
            ret->blue_mask_size = 8;
            ret->blue_mask_shift = 0;
            break;
        case PixelRedGreenBlueReserved8BitPerColor:
            ret->framebuffer_bpp = 32;
            ret->red_mask_size = 8;
            ret->red_mask_shift = 0;
            ret->green_mask_size = 8;
            ret->green_mask_shift = 8;
            ret->blue_mask_size = 8;
            ret->blue_mask_shift = 16;
            break;
        case PixelBitMask:
            if ((mode_info->PixelInformation.RedMask
               | mode_info->PixelInformation.GreenMask
               | mode_info->PixelInformation.BlueMask
               | mode_info->PixelInformation.ReservedMask) == 0) {
                goto out;
            }
            ret->framebuffer_bpp = linear_masks_to_bpp(
                                      mode_info->PixelInformation.RedMask,
                                      mode_info->PixelInformation.GreenMask,
                                      mode_info->PixelInformation.BlueMask,
                                      mode_info->PixelInformation.ReservedMask);
            linear_mask_to_mask_shift(&ret->red_mask_size,
                                      &ret->red_mask_shift,
                                      mode_info->PixelInformation.RedMask);
            linear_mask_to_mask_shift(&ret->green_mask_size,
                                      &ret->green_mask_shift,
                                      mode_info->PixelInformation.GreenMask);
            linear_mask_to_mask_shift(&ret->blue_mask_size,
                                      &ret->blue_mask_shift,
                                      mode_info->PixelInformation.BlueMask);
            break;
        default:
            goto out;
    }

    ret->memory_model = 0x06;
    ret->framebuffer_pitch = mode_info->PixelsPerScanLine * (ret->framebuffer_bpp / 8);
    ret->framebuffer_width = mode_info->HorizontalResolution;
    ret->framebuffer_height = mode_info->VerticalResolution;

    ok = validate_pitch(ret, mode);

out:
    // UEFI calls this buffer callee allocated and says nothing about freeing
    // it, so leave one the protocol is still pointing at alone.
    if (gop->Mode == NULL || mode_info != gop->Mode->Info) {
        gBS->FreePool(mode_info);
    }
    return ok;
}

bool gop_force_16 = false;

// Resolve one EFI_PCI_IO handle to bounded PCI coordinates.  The bounds keep
// every reported source inside PCI numbering; a firmware answer outside them
// is treated as no answer at all.
static bool pci_handle_location(EFI_HANDLE pci_handle,
                                struct fb_pci_source *out) {
    EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_PCI_IO_PROTOCOL *pci_io = NULL;
    EFI_STATUS status = gBS->HandleProtocol(pci_handle, &pci_io_guid,
                                            (void **)&pci_io);
    if (status != EFI_SUCCESS) {
        return false;
    }

    UINTN segment;
    UINTN bus;
    UINTN device;
    UINTN function;
    status = pci_io->GetLocation(pci_io, &segment, &bus, &device, &function);
    if (status != EFI_SUCCESS
     || segment > UINT16_MAX
     || bus > UINT8_MAX
     || device > 31
     || function > 7) {
        return false;
    }

    out->segment = segment;
    out->bus = bus;
    out->device = device;
    out->function = function;
    return true;
}

static bool set_pci_framebuffer_source(struct fb_info *fb,
                                       EFI_HANDLE pci_handle) {
    struct fb_pci_source source;
    if (!pci_handle_location(pci_handle, &source)) {
        return false;
    }

    fb->source_type = FB_SOURCE_PCI;
    fb->pci_source = source;

    printv("gop: Framebuffer source PCI %x:%x:%x.%x\n",
           (uint32_t)source.segment, (uint32_t)source.bus,
           (uint32_t)source.device, (uint32_t)source.function);
    return true;
}

static bool get_framebuffer_source_from_device_path(struct fb_info *fb,
                                                    EFI_HANDLE gop_handle) {
    EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *device_path = NULL;

    EFI_STATUS status = gBS->HandleProtocol(gop_handle, &device_path_guid,
                                            (void **)&device_path);
    if (status != EFI_SUCCESS) {
        return false;
    }
    fb->source_probe |= FB_PROBE_DP_PRESENT;

    EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE pci_handle = NULL;
    status = gBS->LocateDevicePath(&pci_io_guid, &device_path, &pci_handle);
    if (status != EFI_SUCCESS) {
        return false;
    }
    fb->source_probe |= FB_PROBE_DP_PCI_PREFIX;

    if (!set_pci_framebuffer_source(fb, pci_handle)) {
        return false;
    }
    fb->source_probe |= FB_PROBE_DP_LOCATED;
    return true;
}

static bool pci_handle_owns_gop_child(EFI_HANDLE pci_handle,
                                      EFI_GUID *pci_io_guid,
                                      EFI_HANDLE gop_handle) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN entries_count = 0;
    EFI_STATUS status = gBS->OpenProtocolInformation(pci_handle, pci_io_guid,
                                                     &entries, &entries_count);
    if (status != EFI_SUCCESS) {
        return false;
    }

    bool owns = false;
    for (UINTN i = 0; i < entries_count; i++) {
        if ((entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)
         && entries[i].ControllerHandle == gop_handle) {
            owns = true;
            break;
        }
    }
    if (entries != NULL) {
        gBS->FreePool(entries);
    }
    return owns;
}

// Some firmware installs GOP on a child handle whose device path cannot be
// resolved back to EFI_PCI_IO_PROTOCOL. UEFI bus drivers still record the
// controller relationship by opening the parent PCI I/O protocol with
// EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER and the GOP handle as
// ControllerHandle. Walk only those explicit relationships: guessing from
// framebuffer addresses or display class would not prove boot ownership.
static bool get_framebuffer_source_from_child_relation(struct fb_info *fb,
                                                       EFI_HANDLE gop_handle) {
    EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE tmp_handles[1];
    EFI_HANDLE *handles = tmp_handles;
    UINTN handles_size = sizeof(tmp_handles);
    EFI_STATUS status = gBS->LocateHandle(ByProtocol, &pci_io_guid, NULL,
                                          &handles_size, handles);
    if (status != EFI_SUCCESS && status != EFI_BUFFER_TOO_SMALL) {
        return false;
    }

    UINTN handles_alloc = handles_size;
    handles = ext_mem_alloc(handles_alloc);
    status = gBS->LocateHandle(ByProtocol, &pci_io_guid, NULL,
                               &handles_size, handles);
    if (status != EFI_SUCCESS) {
        pmm_free(handles, handles_alloc);
        return false;
    }
    fb->source_probe |= FB_PROBE_CHILD_SCAN;

    bool found = false;
    size_t handles_count = handles_size / sizeof(EFI_HANDLE);
    for (size_t i = 0; i < handles_count; i++) {
        if (pci_handle_owns_gop_child(handles[i], &pci_io_guid, gop_handle)) {
            fb->source_probe |= FB_PROBE_CHILD_RELATION;
            if (set_pci_framebuffer_source(fb, handles[i])) {
                found = true;
                break;
            }
        }
    }
    pmm_free(handles, handles_alloc);
    return found;
}

// The console-splitter posture: firmware that virtualizes console output
// installs GOP on an aggregate handle with no device path and no recorded
// PCI parent, while the handle IS the system table's ConsoleOutHandle.  For
// exactly that handle, firmware's own ConOut variable names the sink device
// paths, and resolving those through EFI_PCI_IO is a firmware-asserted
// binding, not a guess.  All resolved instances must agree on one function:
// a console scanning out via two PCI devices proves ownership of neither.
static bool get_framebuffer_source_from_conout(struct fb_info *fb,
                                               EFI_HANDLE gop_handle) {
    if (gST->ConsoleOutHandle == NULL
     || gop_handle != gST->ConsoleOutHandle) {
        return false;
    }
    fb->source_probe |= FB_PROBE_CONSOLE_HANDLE;

    EFI_GUID global_variable = EFI_GLOBAL_VARIABLE;
    UINTN size = 0;
    EFI_STATUS status = gRT->GetVariable(L"ConOut", &global_variable, NULL,
                                         &size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || size < 4) {
        return false;
    }
    uint8_t *paths = ext_mem_alloc(size);
    UINTN paths_alloc = size;
    status = gRT->GetVariable(L"ConOut", &global_variable, NULL, &size, paths);
    if (status != EFI_SUCCESS || size != paths_alloc) {
        pmm_free(paths, paths_alloc);
        return false;
    }

    // Walk the multi-instance device path node by node.  Node lengths are
    // read bytewise: instance boundaries do not keep the header alignment.
    bool walked = false;
    bool resolved = false;
    bool conflict = false;
    struct fb_pci_source agreed = {0};

    UINTN at = 0;
    UINTN instance_at = 0;
    while (at + 4 <= size) {
        uint8_t type = paths[at];
        uint8_t subtype = paths[at + 1];
        UINTN length = (UINTN)paths[at + 2] | ((UINTN)paths[at + 3] << 8);
        if (length < 4 || length > size - at) {
            break;
        }
        UINTN node_at = at;
        at += length;
        if (type != 0x7f) {
            continue;
        }
        // 0x01 ends one instance, 0xff ends the whole list; anything else
        // is malformation.  Resolve the instance that just closed.
        //
        // LocateDevicePath stops only at an end-of-ENTIRE-path node, so an
        // instance closed by 0x01 would let it read into the next instance.
        // The buffer is our own copy of the variable, so promote this end
        // node for the call and put it back afterwards.
        if (subtype == 0x01) {
            paths[node_at + 1] = 0xff;
        }
        EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
        EFI_DEVICE_PATH *remaining =
            (EFI_DEVICE_PATH *)(paths + instance_at);
        EFI_HANDLE pci_handle = NULL;
        instance_at = at;
        if (gBS->LocateDevicePath(&pci_io_guid, &remaining, &pci_handle)
         == EFI_SUCCESS) {
            struct fb_pci_source candidate;
            if (pci_handle_location(pci_handle, &candidate)) {
                if (!resolved) {
                    resolved = true;
                    agreed = candidate;
                } else if (agreed.segment != candidate.segment
                        || agreed.bus != candidate.bus
                        || agreed.device != candidate.device
                        || agreed.function != candidate.function) {
                    conflict = true;
                }
            }
        }
        if (subtype == 0x01) {
            paths[node_at + 1] = 0x01;
            continue;
        }
        walked = subtype == 0xff && at == size;
        break;
    }
    pmm_free(paths, paths_alloc);

    if (!walked) {
        return false;
    }
    fb->source_probe |= FB_PROBE_CONOUT_VAR;
    if (!resolved) {
        return false;
    }
    fb->source_probe |= FB_PROBE_CONOUT_PCI;
    if (conflict) {
        return false;
    }
    fb->source_probe |= FB_PROBE_CONOUT_UNIQUE;

    fb->source_type = FB_SOURCE_PCI;
    fb->pci_source = agreed;
    printv("gop: Framebuffer source PCI %x:%x:%x.%x (ConOut console)\n",
           (uint32_t)agreed.segment, (uint32_t)agreed.bus,
           (uint32_t)agreed.device, (uint32_t)agreed.function);
    return true;
}

static void get_framebuffer_source(struct fb_info *fb, EFI_HANDLE gop_handle,
                                   size_t handles_count, size_t handle_index) {
    if (!get_framebuffer_source_from_device_path(fb, gop_handle)
     && !get_framebuffer_source_from_child_relation(fb, gop_handle)
     && !get_framebuffer_source_from_conout(fb, gop_handle)) {
        printv("gop: No positive framebuffer source (probe %x)\n",
               fb->source_probe);
    }
    if (fb->source_type == FB_SOURCE_PCI) {
        fb->source_probe |= FB_PROBE_SOURCE_PCI;
    }
    fb->source_probe |= (uint32_t)(handles_count > 0xff ? 0xff : handles_count)
                        << FB_PROBE_HANDLES_SHIFT;
    fb->source_probe |= (uint32_t)(handle_index > 0xff ? 0xff : handle_index)
                        << FB_PROBE_INDEX_SHIFT;
}

static bool try_mode(struct fb_info *ret, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                     size_t mode, uint64_t width, uint64_t height, int bpp,
                     struct fb_info *fbs, size_t fbs_count) {
    EFI_STATUS status;

    if (!mode_to_fb_info(ret, gop, mode)) {
        return false;
    }

    if (width != 0 && height != 0 && bpp != 0) {
        if (ret->framebuffer_width != width
         || ret->framebuffer_height != height
         || ret->framebuffer_bpp != bpp) {
            return false;
        }
    }

    if (gop_force_16) {
        if (ret->framebuffer_width >= 65536
         || ret->framebuffer_height >= 65536
         || ret->framebuffer_pitch >= 65536) {
            return false;
        }
    }

    for (size_t i = 0; i < fbs_count; i++) {
        if (gop->Mode->FrameBufferBase == fbs[i].framebuffer_addr) {
            return false;
        }
    }

    printv("gop: Found matching mode %X, attempting to set...\n", (uint64_t)mode);

    if (mode == gop->Mode->Mode) {
        printv("gop: Mode was already set, perfect!\n");
    } else {
        status = gop->SetMode(gop, mode);

        if (status) {
            printv("gop: Failed to set video mode %X, moving on...\n", (uint64_t)mode);
            return false;
        }
    }

    // Recalculate pitch from gop->Mode->Info, as some firmware (e.g. Apple
    // Macs) report incorrect PixelsPerScanLine via QueryMode.
    ret->framebuffer_pitch = gop->Mode->Info->PixelsPerScanLine * (ret->framebuffer_bpp / 8);

    if (!validate_pitch(ret, mode)) {
        return false;
    }

    ret->framebuffer_addr = gop->Mode->FrameBufferBase;

    return true;
}

static struct fb_info *get_mode_list(size_t *count, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    UINTN modes_count = gop->Mode->MaxMode;

    struct fb_info *ret = ext_mem_alloc_counted(modes_count, sizeof(struct fb_info));

    size_t actual_count = 0;
    for (size_t i = 0; i < modes_count; i++) {
        if (mode_to_fb_info(&ret[actual_count], gop, i)) {
            actual_count++;
        }
    }

    struct fb_info *tmp = ext_mem_alloc_counted(actual_count, sizeof(struct fb_info));
    memcpy(tmp, ret, actual_count * sizeof(struct fb_info));

    pmm_free(ret, modes_count * sizeof(struct fb_info));
    ret = tmp;

    *count = actual_count;
    return ret;
}

#define MAX_PRESET_MODES 128
no_unwind static int preset_modes[MAX_PRESET_MODES];
no_unwind static bool preset_modes_initialised = false;

void init_gop(struct fb_info **ret, size_t *_fbs_count,
              uint64_t target_width, uint64_t target_height, uint16_t target_bpp) {
    if (preset_modes_initialised == false) {
        for (size_t i = 0; i < MAX_PRESET_MODES; i++) {
            preset_modes[i] = -1;
        }
        preset_modes_initialised = true;
    }

    EFI_STATUS status;

    EFI_HANDLE tmp_handles[1];

    EFI_HANDLE *handles = tmp_handles;
    UINTN handles_size = sizeof(EFI_HANDLE);
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    status = gBS->LocateHandle(ByProtocol, &gop_guid, NULL, &handles_size, handles);

    if (status != EFI_SUCCESS && status != EFI_BUFFER_TOO_SMALL) {
        *ret = NULL;
        *_fbs_count = 0;
        return;
    }

    UINTN handles_alloc = handles_size;
    handles = ext_mem_alloc(handles_alloc);

    status = gBS->LocateHandle(ByProtocol, &gop_guid, NULL, &handles_size, handles);
    if (status != EFI_SUCCESS) {
        pmm_free(handles, handles_alloc);
        *ret = NULL;
        *_fbs_count = 0;
        return;
    }

    size_t handles_count = handles_size / sizeof(EFI_HANDLE);

    *ret = ext_mem_alloc_counted(handles_count, sizeof(struct fb_info));

    const struct resolution fallback_resolutions[] = {
        { 0,    0,   0  },   // Overridden by EDID
        { 0,    0,   0  },   // Overridden by preset
        { 1024, 768, 32 },
        { 800,  600, 32 },
        { 640,  480, 32 },
        { 1024, 768, 24 },
        { 800,  600, 24 },
        { 640,  480, 24 },
        { 1024, 768, 16 },
        { 800,  600, 16 },
        { 640,  480, 16 }
    };

    size_t fbs_count = 0;
    for (size_t i = 0; i < handles_count && i < MAX_PRESET_MODES; i++) {
        struct fb_info *fb = &(*ret)[fbs_count];

        uint64_t _target_width = target_width;
        uint64_t _target_height = target_height;
        uint64_t _target_bpp = target_bpp;

        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

        status = gBS->HandleProtocol(handles[i], &gop_guid, (void **)&gop);
        if (status != EFI_SUCCESS) {
            continue;
        }

        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
        UINTN mode_info_size;

        status = gop->QueryMode(gop, gop->Mode == NULL ? 0 : gop->Mode->Mode,
                                &mode_info_size, &mode_info);

        if (status == EFI_NOT_STARTED) {
            if (fbs_count > 0) {
                continue;
            }
            status = gop->SetMode(gop, 0);
            if (status) {
                continue;
            }
            status = gop->QueryMode(gop, gop->Mode == NULL ? 0 : gop->Mode->Mode,
                                    &mode_info_size, &mode_info);
        }

        if (status) {
            continue;
        }

        uint32_t mode_width = mode_info->HorizontalResolution;
        uint32_t mode_height = mode_info->VerticalResolution;
        if (gop->Mode == NULL || mode_info != gop->Mode->Info) {
            gBS->FreePool(mode_info);
        }

        if (preset_modes[i] == -1) {
            preset_modes[i] = gop->Mode->Mode;
        }

        fb->edid = get_edid_info(handles[i]);

        UINTN modes_count = gop->Mode->MaxMode;

        size_t current_fallback = 0;

        if (!_target_width || !_target_height || !_target_bpp) {
            goto fallback;
        } else {
            printv("gop: Requested resolution of %ux%ux%u\n",
                   _target_width, _target_height, _target_bpp);
        }

retry:
        for (size_t j = 0; j < modes_count; j++) {
            if (try_mode(fb, gop, j, _target_width, _target_height, _target_bpp, *ret, fbs_count)) {
                goto success;
            }
        }

fallback:
        if (current_fallback == 0) {
            current_fallback++;

            if (fb->edid != NULL) {
                uint64_t edid_width = (uint64_t)fb->edid->det_timing_desc1[2];
                         edid_width += ((uint64_t)fb->edid->det_timing_desc1[4] & 0xf0) << 4;
                uint64_t edid_height = (uint64_t)fb->edid->det_timing_desc1[5];
                         edid_height += ((uint64_t)fb->edid->det_timing_desc1[7] & 0xf0) << 4;
                if (edid_width >= mode_width
                 && edid_height >= mode_height) {
                    _target_width = edid_width;
                    _target_height = edid_height;
                    _target_bpp = 32;
                    goto retry;
                }
            }
        }

        if (current_fallback == 1) {
            current_fallback++;

            if (try_mode(fb, gop, preset_modes[i], 0, 0, 0, *ret, fbs_count)) {
                goto success;
            }
        }

        if (current_fallback < SIZEOF_ARRAY(fallback_resolutions)) {
            _target_width = fallback_resolutions[current_fallback].width;
            _target_height = fallback_resolutions[current_fallback].height;
            _target_bpp = fallback_resolutions[current_fallback].bpp;

            current_fallback++;
            goto retry;
        }

        if (fb->edid != NULL) {
            pmm_free(fb->edid, sizeof(struct edid_info_struct));
            fb->edid = NULL;
        }

        continue;

success:;
        size_t mode_count;
        fb->mode_list = get_mode_list(&mode_count, gop);
        fb->mode_count = mode_count;
        get_framebuffer_source(fb, handles[i], handles_count, i);

        fbs_count++;
    }

    pmm_free(handles, handles_alloc);

    gop_force_16 = false;

    *_fbs_count = fbs_count;
}

#endif
