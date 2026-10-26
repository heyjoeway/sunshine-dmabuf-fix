# sunshine-dmabuf-fix

A workaround for **garbled video when Sunshine uses Vulkan hardware encoding
inside a VM with AMD virtio-gpu native context** (`drm_native_context=on`,
Mesa built with `-Damdgpu-virtio=true`). The garbling shows up as horizontal
smearing or streaks. Frame rate and latency stay fine, and software encoding
of the same capture is clean.

It installs a small Vulkan layer and keeps it correct across system updates.
The layer switches itself off whenever it isn't needed or stops helping.

Supported and tested, each with a clean Moonlight stream:

| System | Host GPU | Desktop | Sunshine |
|---|---|---|---|
| Ubuntu 26.04 LTS | Renoir iGPU | GNOME on Wayland | `.deb` 2026.914 |
| Bazzite (deck image, Fedora 44 base) | RX 6700 XT (Navi22) | Gamescope and KDE Plasma | Homebrew `sunshine-beta` 2026.1018 |

Both run as Proxmox VE 9 guests. Bazzite has a few problems of its own that
this layer doesn't fix; see [Bazzite notes](#bazzite-notes).

The repo also includes [`edid/`](edid/), a separate tool that gives the VM
high-resolution, high-refresh display modes (up to 7680x2160, 60–360 Hz,
ultrawides included); see [High-refresh modes](#high-refresh-modes-custom-edid).

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
git clone --recurse-submodules https://github.com/heyjoeway/sunshine-dmabuf-fix.git
cd sunshine-dmabuf-fix
./sunshine-dmabuf-fix build          # as your user
sudo ./sunshine-dmabuf-fix install
```

- **Ubuntu** needs `gcc` (`sudo apt install gcc`).
- **Bazzite** ships `gcc` on the host, and that's the tested path. If no
  compiler is found, `build` falls back to compiling in a throwaway Fedora
  container with `podman`. That fallback is untested. Run `build` as your user
  so the build output isn't owned by root.

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
| self-test times out (each run is limited to 20 s) | GPU busy, e.g. during boot | unchanged |
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

- **Homebrew Sunshine (Bazzite) is checked only indirectly.** The self-test
  exercises the system Vulkan loader and Mesa, not Homebrew's. In practice,
  Homebrew's loader also finds Bazzite's system RADV, and the layer engages.
  The `active` line in Sunshine's log confirms it.
- **The podman build fallback is untested.**
- **Flatpak Sunshine can't see host layers.** It would need this packaged as
  a Flatpak extension.
- The self-test approximates the compositor's buffer with a GBM scanout
  allocation. It doesn't capture the real screen, because that needs
  `CAP_SYS_ADMIN` and a running session.

## Bazzite notes

Bazzite installs Sunshine from Homebrew (`ujust setup-sunshine` →
`lizardbyte/homebrew/sunshine-beta`). Getting a clean stream in a
native-context VM also took the fixes below. None of them involve this layer.

- **Use a Sunshine build that includes
  [LizardByte/Sunshine#5675](https://github.com/LizardByte/Sunshine/pull/5675).**
  2026.1018 has it; 2026.907 doesn't. Older builds crash on the first frame
  with `vkQueueSubmit: Invalid queue` in `dispatch_compute`, and Moonlight
  shows a black screen with any encoder. Bazzite pins the formula, so upgrade
  with:
  ```sh
  brew unpin lizardbyte/homebrew/sunshine-beta
  brew upgrade lizardbyte/homebrew/sunshine-beta
  brew pin lizardbyte/homebrew/sunshine-beta
  ```
- **Re-run `postinst` after every Sunshine upgrade.** Without it, KMS capture
  loses `CAP_SYS_ADMIN`. The log shows `Failed to gain CAP_SYS_ADMIN`, and
  Moonlight fails with error 503.
  ```sh
  sudo "$(brew --prefix sunshine-beta)/bin/postinst"
  ```
- **Moonlight can't find the host.** The log shows
  `Failed to create client: Daemon not running`. Homebrew's `libdbus` looks for
  the system bus under the Homebrew prefix, so Avahi publishing fails. Sunshine
  has file capabilities, so `DBUS_SYSTEM_BUS_ADDRESS` is ignored. Point the
  Homebrew path at the real bus, then restart Sunshine:
  ```sh
  ln -sfn /run/dbus/system_bus_socket "$(brew --prefix)/var/run/dbus/system_bus_socket"
  systemctl --user restart app-dev.lizardbyte.app.Sunshine
  ```
- **Set `encoder = vulkan`.** VA-API doesn't work under native context.
  Sunshine falls back to Vulkan by itself, but only after a failed probe.
- **Flatpak browsers crash** with `importing the supplied dmabufs failed`. The
  Flatpak runtime's Mesa is built without `amdgpu-virtio`. Workarounds:
  - Firefox: `flatpak override --user --env=LIBGL_ALWAYS_SOFTWARE=1 org.mozilla.firefox`
  - Chrome: add `--disable-gpu` to `~/.var/app/com.google.Chrome/config/chrome-flags.conf`
- **"Switch to Desktop" loops back to Gamescope.** This happened in a VM whose
  hardware clock was in local time: a file in `/etc/sddm.conf.d` got a future
  timestamp. SDDM only reloads its config when a file there is newer than the
  newest one it has seen, so it ignored the switch.
  ```sh
  find /etc/sddm.conf.d -newermt now                      # list future-dated files
  sudo timedatectl set-local-rtc 0                        # keep the RTC in UTC
  sudo touch /etc/sddm.conf.d/* && sudo systemctl restart sddm
  ```
  The stale file in this case was `99-plasma-setup.conf`, left behind after
  `plasma-setup` was removed from the image. Deleting it is fine.

## High-refresh modes (custom EDID)

A virtio-gpu VM only offers the modes in an EDID that QEMU generates: a
1280x800 @ 75 Hz preferred mode, everything else at 50–60 Hz, and a range
capped at 125 Hz. Neither QEMU nor Proxmox can change those refresh rates or
supply your own EDID. [`edid/`](edid/) builds a replacement EDID and has the
kernel load it at boot with `drm.edid_firmware=`. Every compositor then sees
the same mode list: KDE's display settings, Gamescope, and Steam's resolution
and refresh pickers in Game Mode.

```sh
edid/vm-edid build                    # as your user; writes build/vm-edid.bin
sudo edid/vm-edid install             # then reboot
edid/vm-edid status                   # after the reboot
```

The boot-time approach was tested on Bazzite in Gamescope, using an earlier
six-mode EDID (4K120, 1440p120/240). The full default matrix below passes
`edid-decode` and libdisplay-info (the parser KWin and Gamescope use) with no
warnings, but hasn't been booted on either system yet.

### Default modes

Every resolution at every refresh rate (156 modes), plus the preferred mode, as CVT reduced
blanking v2 timings:

| Aspect | Resolutions |
|---|---|
| 16:9 | 1920x1080, 2560x1440, 3840x2160, 5120x2880 |
| 16:10 | 1920x1200, 2560x1600 |
| 21:9 and similar | 2560x1080, 3440x1440, 3840x1600, 5120x2160 |
| 32:9 | 3840x1080, 5120x1440, 7680x2160 |

Refresh rates: 60, 75, 100, 120, 144, 165, 175, 180, 200, 240, 280 and 360 Hz.
The desktop starts at the preferred mode, 1024x768 @ 60. Pick your real mode
once in KDE's display settings or Steam's display settings; both remember it
per monitor, so later boots come up in it.

**Why 1024x768:** virtio-gpu deletes any mode flagged preferred unless it's
1024x768 or within 16 pixels of the scanout size QEMU reports. That size starts
as the device's `xres`/`yres` (1280x800 unless set) but can follow the console
client, for example noVNC with remote resizing. Without a preferred mode,
compositors and the boot console start at the biggest mode in the list: here
that was 7680x2160 @ 360 Hz, which left the boot screen panned and coincided
with boot taking minutes. 1024x768 is the one size that's always kept. For a
different preferred mode, pin the scanout size on the host and tell the
generator; the generator refuses a preferred mode that would be deleted:

```sh
# Proxmox host, /etc/pve/qemu-server/<vmid>.conf (append to any existing args:)
args: -set device.vga.xres=2560 -set device.vga.yres=1440
```
```sh
# VM
edid/vm-edid build --preferred 2560x1440@120 --host-size 2560x1440
```

Customize with `build` options (see `edid/make_edid.py --help`):

```sh
edid/vm-edid build --resolutions 2560x1440,3440x1440 --rates 60,120,165,240
edid/vm-edid build --max-clock-mhz 2000   # drop the most extreme modes
python3 edid/make_edid.py --list          # print the modes without writing a file
```

### How it works

- **EDID layout:**
  - The base EDID block carries only the preferred mode. Its detailed timing
    format tops out at a 655 MHz pixel clock.
  - Every other mode goes into DisplayID 1.3 extension blocks as Type I
    timings, five per 128-byte block. The default matrix is 33 blocks (4 KB).
  - DisplayID 2.0 Type VII timings would also work for the kernel, but
    libdisplay-info doesn't decode them, so they aren't used.
- **No inferred modes:** there's no range-limits descriptor and no
  "continuous frequency" flag. Without them the kernel adds no inferred
  in-between modes, so the mode list is exactly the matrix.
- **The runtime override doesn't work:** virtio-gpu reads its EDID only when
  the driver starts or when the host signals a display change. The debugfs
  `edid_override` entry, followed by a `detect`, is accepted but never used.
  So the EDID has to be loaded at boot.

### What `install` does

| | Ubuntu 26.04 | Bazzite |
|---|---|---|
| EDID file | `/usr/lib/firmware/edid/vm-edid.bin` | `/etc/firmware/edid/vm-edid.bin` (`/usr` is read-only) |
| Kernel arguments | `/etc/default/grub.d/99-vm-edid.cfg`, then `update-grub` | `rpm-ostree kargs`: `drm.edid_firmware=…` and `firmware_class.path=/etc/firmware` |
| Initramfs | hook `/etc/initramfs-tools/hooks/vm-edid`, then `update-initramfs -u` | `rpm-ostree initramfs-etc --track` (virtio-gpu is built into the kernel and starts early) |

The connector is detected automatically: it's the one connected `Virtual-N`
output; override it with `--connector`.

On Bazzite the file is named after its content (`vm-edid-<hash>.bin`).
`rpm-ostree initramfs-etc` keeps reusing its initramfs overlay while the set of
tracked paths stays the same, and `--force-sync` did not pick up a changed file
in testing: the kernel kept loading the first EDID ever installed. A new name
per EDID forces a new overlay. Reinstalling replaces the kernel argument,
untracks other EDIDs under `/etc/firmware/edid/`, and deletes this tool's
earlier files.

After rebooting, `edid/vm-edid status` shows the EDID the kernel actually
loaded (name, preferred mode, resolution count) and whether it matches the
installed file.

### Surviving updates

- **Ubuntu:**
  - The grub drop-in and the firmware file aren't owned by any package.
  - The initramfs hook copies the EDID into every rebuilt initramfs.
- **Bazzite:** kernel arguments and `initramfs-etc` files carry over to every
  new deployment.
- **Different hardware:** the argument names the virtio-gpu connector, so on
  other hardware (for example GPU passthrough) it matches nothing and does
  nothing.

### Limitations

- **The refresh rate is a target on virtio-gpu, not a guarantee.** There's no
  real scanout clock: a page flip completes when the host has processed the
  frame. The compositor paces itself to the mode's refresh rate, but the
  achieved frame rate depends on the host. With Proxmox's VNC console, QEMU
  also reads every frame back for the console. If a high mode doesn't reach
  its rate in Moonlight's statistics, check the QEMU process's CPU on the host.
- **The top modes are there for completeness.** 7680x2160 @ 360 Hz is a
  7.2 GHz pixel clock. Use `--max-clock-mhz` or a shorter list if the menus
  get too long.
- Changing modes still needs a reboot, because of the runtime limitation
  above.

## Layout

```
layer/dmabuf_dedicated_layer.c   the Vulkan layer
selftest/dmabuf_selftest.c       the import self-test
share/                           manifest, systemd unit, apt hook, config templates
sunshine-dmabuf-fix              build/install/check/status/uninstall
edid/make_edid.py                EDID generator for high-refresh VM modes
edid/vm-edid                     build/install/uninstall/status for the EDID
third_party/Vulkan-Headers       submodule, pinned to v1.4.366 (headers only)
```

The layer and self-test load `libvulkan.so.1` and `libgbm.so.1` at runtime.
Building needs only a C compiler and the submodule.

## License

MIT, see [LICENSE](LICENSE). Vulkan-Headers (submodule) is Apache-2.0 OR MIT.
