# espweb — browser-based firmware installer

## What this is

This directory is the source for [espweb.terazone.com](https://espweb.terazone.com/), a static site plus a Cloudflare Pages Function that serves an [ESP Web Tools](https://esphome.github.io/esp-web-tools/) installer for this repo's HomeKit firmware. The Pages Function reads a small firmware manifest and binary firmware files out of an R2 bucket (`gdo-blaq-firmware`, bound as `FIRMWARE`), which GitHub Actions keeps in sync on every non-prerelease GitHub Release published from this repo.

## Local development

```sh
cd espweb
npm install
npm run dev
```

`npm run dev` runs `wrangler pages dev` and serves `site/` with the Pages Function in `functions/` attached, so you can exercise the `/firmware/*` routes locally.

## Deploying

**Site content changes (`espweb/site/**`, `espweb/functions/**`) are NOT deployed automatically on merge.** The `gdo-blaq-espweb` Cloudflare Pages project uses direct upload, not git integration, so merging to `main` does not by itself publish anything to espweb.terazone.com.

After merging a change under `espweb/site/` or `espweb/functions/`, someone must deploy it by hand:

```sh
cd espweb
npm run deploy
```

which runs `wrangler pages deploy` (equivalent to `npx wrangler pages deploy site --project-name=gdo-blaq-espweb`). This requires a Cloudflare account with access to the `gdo-blaq-espweb` Pages project.

## CI secrets

`.github/workflows/build.yml`'s `release` job publishes firmware to R2 on every published (non-prerelease) GitHub Release, so that the installer above always points at the latest build. This needs two repository secrets:

- `CLOUDFLARE_API_TOKEN` — scoped to R2 write access for the `gdo-blaq-firmware` bucket.
- `CLOUDFLARE_ACCOUNT_ID` — the Cloudflare account ID that owns the bucket.

If these secrets are missing, expired, or wrong, the R2 publish step now fails gracefully (`continue-on-error`) instead of failing the whole release — the GitHub Release itself (with its `.bin` assets) still gets created normally. However, firmware won't reach the web installer until the secrets are fixed and a new release is published, since the installer's manifest is only ever updated by this step.

## Infrastructure

- **R2 bucket:** `gdo-blaq-firmware` — holds `current.json` (the version/filename pointer) and the firmware binaries themselves.
- **Cloudflare Pages project:** `gdo-blaq-espweb` — hosts this site, direct-upload only (no git integration).
- **Custom domain:** `espweb.terazone.com`, mapped to the Pages project.
