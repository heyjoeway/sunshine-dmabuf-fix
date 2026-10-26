# sunshine-dmabuf-fix

A workaround for **garbled video when Sunshine uses Vulkan hardware encoding
inside a VM with AMD virtio-gpu native context** (`drm_native_context=on`,
Mesa built with `-Damdgpu-virtio=true`). The garbling shows up as horizontal
smearing or streaks. Frame rate and latency stay fine, and software encoding
of the same capture is clean.

It installs a small Vulkan layer and keeps it correct across system updates.
The layer switches itself off whenever it isn't needed or stops helping.

> [!WARNING]
> **Bazzite support is planned but untested.** Everything here has been
> verified on Ubuntu 26.04 LTS only: a Proxmox VE 9 host, a Renoir iGPU and
> GNOME on Wayland. The Bazzite install path (container build, Homebrew
> Sunshine) is written but has never been run. See [Limitations](#limitations),
> and please open an issue with your results if you try it.

Supported: **Ubuntu 26.04 LTS** (tested). **Bazzite** is planned; the
installer accepts it, but it's untested.

## The bug

On virtio-gpu, the KMS framebuffers that GNOME/KDE scan out have no DRM
format modifier (`DRM_CAP_ADDFB2_MODIFIERS = 0`). radeonsi still allocates
them **tiled**, so the layout exists only in the buffer object's metadata.
With a missing modifier, Sunshine's Vulkan encoder (`vulkan_encode.cpp`,
`import_dmabuf`) imports the buffer as `VK_IMAGE_TILING_LINEAR` without a
dedicated allocation. RADV reads tiling metadata only for **dedicated**
imports, so it samples tiled memory as linear and the stream is garbled.

The layer adds `VkMemoryDedicatedAllocateInfo` to those imports. RADV then
picks up the real tiling, and the capture → compute → encode path stays
zero-copy on the GPU.

The proper fix belongs in Sunshine, and the same two-line change would make
this repo unnecessary.

## Install

```sh
git clone --recurse-submodules https://github.com/<owner>/sunshine-dmabuf-fix.git
cd sunshine-dmabuf-fix
./sunshine-dmabuf-fix build          # as your user
sudo ./sunshine-dmabuf-fix install
```

- **Ubuntu** needs `gcc` (`sudo apt install gcc`).
- **Bazzite** has no compiler on the host. `build` uses `podman` to compile
  in a throwaway Fedora container, or you can run it inside a toolbox or
  distrobox. Run `build` as your user so the build output isn't owned by root.

Sunshine uses the layer from its next stream onwards. Then check:

```sh
sunshine-dmabuf-fix status           # or /usr/local/lib/sunshine-dmabuf-fix/sunshine-dmabuf-fix status
journalctl --user -u app-dev.lizardbyte.app.Sunshine | grep dmabuf_dedicated
```

When the layer engages, Sunshine's output includes
`[dmabuf_dedicated] active: RADV on virtio_gpu (native context)`.

## What gets installed

| Path | Purpose |
|---|---|
| `/usr/local/lib/sunshine-dmabuf-fix/` | Snapshot: layer `.so`, self-test, this script, manifest template, `installed` (version info) |
| `/etc/vulkan/implicit_layer.d/VkLayer_LOCAL_dmabuf_dedicated.json` | Present only while the layer is **enabled** |
| `/etc/systemd/system/sunshine-dmabuf-fix.service` | Re-check at every boot |
| `/etc/apt/apt.conf.d/99sunshine-dmabuf-fix` | Re-check after every apt/dpkg run (Ubuntu only) |
| `/etc/sunshine-dmabuf-fix.conf` | `MODE=auto\|on\|off` |
| `/var/lib/sunshine-dmabuf-fix/state` | Result and reason of the last check |

## How it survives updates

Nothing is pulled from this repository after install. Three things make the
install durable instead:

1. **It lives outside package-managed paths.** `/usr/local`, `/etc` and
   `/var` are never written by apt, dpkg, rpm-ostree or Homebrew.
   - On Ubuntu no package owns these files, so upgrades leave them alone.
   - On Bazzite, `/usr` is replaced on each image update, but `/usr/local`
     (actually `/var/usrlocal`) and `/var` carry over unchanged, and `/etc`
     is three-way merged, which keeps local files.
2. **It doesn't modify Sunshine or Mesa.** Patching the Sunshine binary
   would be undone by every Sunshine update, the way its `setcap` is. A
   Vulkan layer is a separate file that the Vulkan loader picks up by
   itself, including for:
   - Sunshine with file capabilities, where the loader ignores environment
     variables but still reads `/etc/vulkan`.
   - Homebrew's Vulkan loader on Bazzite, which also searches `/etc`.
3. **It re-validates itself after anything changes.**
   - The apt hook and the boot service run `sunshine-dmabuf-fix check`
     from the installed snapshot.
   - On Bazzite, OS updates only apply on reboot, so the boot service
     covers them. The service runs before the display manager, so the
     decision is made before Sunshine starts.
   - The self-test uses whatever Mesa and Vulkan loader are installed at
     the time, so a Mesa update is tested on its own merits.

To move to a newer version of this fix, update the repo yourself and
re-run `install`:

```sh
git pull && git submodule update --init && ./sunshine-dmabuf-fix build && sudo ./sunshine-dmabuf-fix install
```

## When it switches itself off

### Every check (install, boot, after apt)

`check` runs `dmabuf-selftest` twice. Both runs ignore every installed
implicit layer, and the second run loads this layer explicitly. The
self-test reproduces Sunshine's exact import:

1. Allocate a compositor-style scanout buffer through GBM: `SCANOUT`,
   implicit modifier.
2. Fill it with a known pattern from the CPU.
3. Import it into RADV the way Sunshine does.
4. Copy it back and compare.

The layer is enabled **only** if the import is wrong without the layer
**and** correct with it. Every other outcome removes the manifest:

| Self-test result | Meaning | Layer |
|---|---|---|
| no `virtio_gpu` render node | not a native-context VM (e.g. bare metal) | off |
| no RADV device | Mesa lacks `amdgpu-virtio`, or not AMD | off |
| buffer has an explicit modifier | Sunshine takes its modifier path, so no bug | off |
| import correct without the layer | bug fixed elsewhere (Mesa/kernel) | off |
| import still wrong with the layer | layer no longer works | off |
| self-test can't run (no GBM, GPU missing, ...) | can't verify | off until a later check passes |
| wrong without, correct with | bug present and fixed | **on** |

`MODE=on` or `MODE=off` in `/etc/sunshine-dmabuf-fix.conf` overrides this.

### Inside every process (runtime gates in the layer)

Even when it's enabled, the layer stays inert unless all of these hold:

- The process name starts with `sunshine`. Every other Vulkan app gets the
  driver's functions directly, with no wrappers.
- The device is RADV (`VK_DRIVER_ID_MESA_RADV`), and its DRM render node is
  driven by `virtio_gpu`.

For each import, it also checks:

- **The app hasn't already asked for a dedicated allocation.** Once
  Sunshine fixes this upstream, the layer becomes a no-op on its own.
- **Exactly one candidate image is waiting.** If the target is ambiguous,
  it does nothing and logs `skipped`.
- **The app binds the memory to that same image at offset 0.** If not, it
  logs `disabled: ... please report this` and stays off for the rest of
  the process.

`DISABLE_DMABUF_DEDICATED_LAYER=1` in the environment stops the loader from
loading the layer at all.

## Commands

```
sunshine-dmabuf-fix build              compile into ./build
sunshine-dmabuf-fix install [--force]  install + hooks + check (root); --force skips the OS check
sunshine-dmabuf-fix check [--quiet] [--wait]
sunshine-dmabuf-fix status
sunshine-dmabuf-fix uninstall          removes everything but the config file (root)
```

Set `SDF_ROOT=/some/dir` to install into a scratch tree for testing. In that
case, systemctl calls are skipped.

## Limitations

- **Bazzite is untested.** Bazzite installs Sunshine from Homebrew
  (`ujust setup-sunshine` → `lizardbyte/homebrew/sunshine-beta`).
  - Homebrew's own Mesa is built without `amdgpu-virtio`, so Sunshine has to
    end up on Bazzite's system RADV, which does have it.
  - Confirm that the `GPU listing` in Sunshine's log shows `RADV` and that the
    `active` line appears.
  - The self-test exercises the system loader and Mesa, not Homebrew's.
- **Flatpak Sunshine can't see host layers.** It would need this packaged as
  a Flatpak extension.
- The self-test approximates the compositor's buffer with a GBM scanout
  allocation. It doesn't capture the real screen, because that needs
  `CAP_SYS_ADMIN` and a running session.

## Layout

```
layer/dmabuf_dedicated_layer.c   the Vulkan layer
selftest/dmabuf_selftest.c       the import self-test
share/                           manifest, systemd unit, apt hook, config templates
sunshine-dmabuf-fix              build/install/check/status/uninstall
third_party/Vulkan-Headers       submodule, pinned to v1.4.366 (headers only)
```

The layer and self-test load `libvulkan.so.1` and `libgbm.so.1` at runtime.
Building needs only a C compiler and the submodule.

## License

MIT, see [LICENSE](LICENSE). Vulkan-Headers (submodule) is Apache-2.0 OR MIT.
