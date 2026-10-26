# sunshine-dmabuf-fix

> [!NOTE]
> This repo is a follow-up to
> [zeroqn/proxmox-native-context](https://github.com/zeroqn/proxmox-native-context),
> which sets up the Proxmox host side of AMD virtio-gpu native context. Start
> there; this repo covers Sunshine streaming inside the guest. For
> high-refresh display modes in the same guests, see
> [heyjoeway/virtio-gpu-edid](https://github.com/heyjoeway/virtio-gpu-edid).

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
| Bazzite (deck image: stable 44.20261023 and testing 45.20261025) | RX 6700 XT (Navi22) | Gamescope and KDE Plasma | Homebrew `sunshine-beta` 2026.1018 |

Both run as Proxmox VE 9 guests. Bazzite has a few problems of its own that
this layer doesn't fix; see [Bazzite notes](#bazzite-notes).

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

## Related

- [zeroqn/proxmox-native-context](https://github.com/zeroqn/proxmox-native-context):
  Proxmox host setup for AMD virtio-gpu native context.
- [heyjoeway/virtio-gpu-edid](https://github.com/heyjoeway/virtio-gpu-edid):
  high-resolution, high-refresh display modes (60–360 Hz, ultrawides included)
  for virtio-gpu VMs. It used to live in this repo's `edid/` directory.

## License

MIT, see [LICENSE](LICENSE). Vulkan-Headers (submodule) is Apache-2.0 OR MIT.
