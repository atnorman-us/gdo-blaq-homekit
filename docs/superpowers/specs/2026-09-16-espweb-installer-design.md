# espweb.terazone.com — browser-based firmware installer

**Date:** 2026-09-16
**Status:** Approved for planning

## Purpose

Give users of this repo's native HomeKit firmware for the GDO blaQ a one-page,
browser-based flashing tool (à la [install.konnected.io](https://install.konnected.io)),
hosted on Cloudflare at `espweb.terazone.com`. Konnected's page covers four
hardware SKUs and three firmware platforms per SKU; this repo only produces
one firmware for one device, so the new page is single-purpose: no
hardware/firmware picker, just the HomeKit build for the GDO blaQ.

Visual reference: Konnected's page shows the currently-selected firmware
platform as a card with a "BETA" pill and a "Works with Apple Home" badge
when HomeKit is selected — that's the specific look being matched, adapted
with original artwork (not Konnected's or Apple's copyrighted/trademarked
assets — see Branding below).

## Non-goals

- No hardware or firmware picker (single product, single firmware).
- No reproduction of Konnected's logo, Adobe Typekit font, or Apple's
  official "Works with Apple Home" badge artwork.
- No changes to the existing GitHub Releases publishing flow — it keeps
  working exactly as it does today; R2 publishing is additive.
- `scripts/update-espwebtools-manifest.rb` is unrelated legacy tooling
  (inherited from upstream Konnected's own internal site repo, not wired
  into this fork's CI). It's out of scope and left untouched.

## Architecture

Two Cloudflare pieces, deployed to the **same origin** so the browser never
makes a cross-origin request for the firmware (GitHub Releases assets lack
CORS headers, confirmed via `curl`, which is why the firmware can't be
fetched by ESP Web Tools directly from GitHub):

1. **Cloudflare Pages** serves the static site (HTML/CSS/vendored ESP Web
   Tools JS) at `espweb.terazone.com`.
2. **A Cloudflare Pages Function** at `/firmware/*` proxies a private
   **R2 bucket** that holds the firmware. It serves:
   - `GET /firmware/manifest.json` — builds the ESP Web Tools manifest on
     the fly from a small `current.json` pointer object in R2.
   - `GET /firmware/<filename>.bin` — streams the matching object from R2.

Both are same-origin with the page, so no CORS configuration is needed
anywhere in this design.

**CI (GitHub Actions):** the existing `release` job in
`.github/workflows/build.yml` keeps uploading to GitHub Releases exactly as
it does today. One new step is added after that: upload the combined image
(`konnected-gdo-blaq-homekit-<tag>.bin`) to the R2 bucket and overwrite
`current.json` (`{"version": "<tag>", "file": "konnected-gdo-blaq-homekit-<tag>.bin"}`).
This needs two new GitHub Actions secrets: an R2 API token (scoped to this
bucket only) and the Cloudflare account ID.

## Data flow

1. Maintainer publishes a GitHub Release (unchanged process).
2. `build.yml`'s `release` job builds the combined image, uploads it to
   GitHub Releases (unchanged), then uploads the same binary plus an
   updated `current.json` to R2 (new step).
3. A visitor loads `espweb.terazone.com`; Pages serves the static site and
   the vendored ESP Web Tools JS (no `unpkg.com` dependency at runtime).
4. The page's `<esp-web-install-button manifest="/firmware/manifest.json">`
   triggers a same-origin fetch to the Pages Function.
5. The Function reads `current.json` from R2 and returns
   `{"name": "...", "version": "<tag>", "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": "/firmware/<file>", "offset": 0}]}]}`.
6. ESP Web Tools fetches `/firmware/<file>` (same Function, streamed from
   R2) over Web Serial and flashes the device — no cross-origin request at
   any point.

## File layout (new files in this repo)

```
site/
  index.html          # page markup: hero, badge card, Connect button, FAQ
  style.css            # original design, no Konnected/Apple assets
  assets/
    logo.svg            # original wordmark for this project
    apple-home-badge.svg  # original badge: house glyph + "Works with Apple Home" text
    favicon.svg
  vendor/
    esp-web-tools/       # pinned copy of the published esp-web-tools package
functions/
  firmware/
    [[path]].js         # Pages Function: manifest.json + .bin proxy from R2
wrangler.toml            # R2 bucket binding for the Pages project
```

CI change: one new step appended to the `release` job in
`.github/workflows/build.yml`.

## Visual design

Single centered column (~1000px max width), plain background, matching the
clean/minimal feel of Konnected's page without copying its assets:

- Hero: original logo/wordmark, H1 ("GDO blaQ — HomeKit Web Installer" or
  similar), one paragraph of instructions (mirrors the README's "Provision
  WiFi and add to HomeKit" framing).
- One card (not a picker) showing:
  - Firmware name and version (from the live manifest, so it's always
    correct without a site redeploy).
  - A small **BETA** pill next to an original "Works with Apple Home"-style
    badge — a house glyph plus text I draw myself, not Apple's trademarked
    mark.
  - The caveats already documented in the README: requires an Apple Home
    hub (HomePod/Apple TV), not Apple-certified, some customization options
    limited.
  - The `esp-web-install-button` Connect flow (Chrome/Edge only, with the
    same "use Chrome or Edge" fallback message for unsupported browsers).
- FAQ/troubleshooting accordion adapted from the README's Troubleshooting
  section (browser support, USB cable/driver notes relevant to the GDO
  blaQ, setup code, WiFi provisioning steps).
- Footer linking back to the GitHub repo and Releases page.
- Vanilla JS only (no jQuery) — the only interactivity is the accordion and
  the ESP Web Tools custom element.

## Error handling

- If R2 or `current.json` is unreachable, the Function returns a clear
  JSON error (5xx) instead of a malformed manifest. The page detects a
  failed manifest fetch and shows a fallback message pointing at the
  GitHub Releases page for manual USB flashing, instead of leaving a
  broken Connect button on screen.
- ESP Web Tools' own `slot="unsupported"` message is kept for non-Chromium
  browsers, matching Konnected's existing UX.

## Provisioning

The user will run `wrangler login` locally (interactive browser auth), then
Claude runs `wrangler pages project create`, `wrangler r2 bucket create`,
and the custom-domain binding via CLI, showing each command before running
it. GitHub Actions secrets (R2 token, account ID) are added by the user via
the GitHub repo settings, or by Claude via `gh secret set` if asked.

## Testing / verification

- Local preview via `wrangler pages dev` (site + Function + R2 binding)
  before deploying.
- Manual check in Chrome: manifest resolves, Connect opens the serial port
  picker, and the proxied `.bin` matches the R2 object's byte length/hash.
  Full end-to-end flashing needs physical GDO blaQ hardware attached, which
  isn't something this session can do — that step is manual verification
  by the user after deploy.
- No new automated test suite: this is a static site plus a small proxy
  function: manual verification covers it.
