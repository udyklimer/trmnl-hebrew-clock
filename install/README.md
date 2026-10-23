# Web installer: notes for the clock server

This folder holds a web page that installs the clock firmware on a new device
from Chrome or Edge, with nothing installed on the computer. It uses
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) and the browser's
Web Serial API. The clock server should host it at `/install`.

## What the server needs to serve

All three must come from the clock server itself. The browser fetches the
manifest and the firmware from the page's own site, and GitHub release
downloads can't be fetched cross-origin, so linking to GitHub directly does
not work.

| Path | Content |
|---|---|
| `GET /install` | `index.html` from this folder, unchanged |
| `GET /install/manifest.json` | `manifest.json` from this folder, with `"version"` set to the latest release's version (tag without the `v`, e.g. `1.0.5`) |
| `GET /install/firmware-full.bin` | the latest release's `firmware-full.bin` asset, unmodified, as `application/octet-stream` |

- The page loads the manifest from the absolute path `/install/manifest.json`,
  and the manifest names the firmware relative to itself, so the firmware
  must be at `/install/firmware-full.bin`.
- Fetch the asset from the latest GitHub release of `udyklimer/trmnl-hebrew-clock`
  and cache it. Refresh it when a new release appears, the same way the server
  already discovers releases for OTA.
- Only releases from `v1.0.5` on have `firmware-full.bin`. Older ones don't.
- The page must be served over https; Web Serial is blocked on plain http.

## Release assets

Each release now has three assets:

| Asset | Used for |
|---|---|
| `firmware.bin` | OTA updates, as before |
| `firmware.bin.sig` | OTA signature, as before |
| `firmware-full.bin` | the web installer only: bootloader, partition table, boot_app0 and app in one image, flashed at offset 0 |

OTA must keep using `firmware.bin` and its signature and must ignore
`firmware-full.bin`. A device can't install `firmware-full.bin` over the air.

## Why it matters that the file is served unmodified

The browser writes whatever it gets straight to the device's flash, with no
signature check; the signature only protects OTA updates. A device installed
this way runs the release version from the tag, so it reports `fw=<version>`
and receives OTA updates from then on.

## Optional

- Link to `/install` from the server's pages, e.g. the sign-up or help page.
- The page is in English. It can be translated; keep the
  `<esp-web-install-button>` element and its `manifest` attribute as they are.
