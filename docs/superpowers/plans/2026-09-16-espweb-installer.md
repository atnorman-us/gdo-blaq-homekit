# espweb.terazone.com Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a single-purpose, browser-based ESP Web Tools flasher for this repo's native HomeKit firmware, hosted on Cloudflare Pages at `espweb.terazone.com`, with firmware served same-origin from an R2 bucket that GitHub Actions keeps in sync with each GitHub Release.

**Architecture:** A Cloudflare Pages project serves a static site (`espweb/site/`) plus a Pages Function (`espweb/functions/firmware/[[path]].js`) that proxies an R2 bucket under `/firmware/*` — same origin as the page, so no CORS is needed anywhere. The existing `release` job in `.github/workflows/build.yml` gets one new step that pushes the combined firmware image and a small `current.json` pointer to R2 after every GitHub Release, so the site always serves the latest firmware without a redeploy.

**Tech Stack:** Cloudflare Pages, Pages Functions (vanilla JS, Workers runtime), Cloudflare R2, Wrangler CLI, vanilla HTML/CSS/JS (no framework, no build step), vendored ESP Web Tools 10.4.0 (Apache-2.0), GitHub Actions.

## Global Constraints

- No hardware/firmware picker — this repo produces one firmware for one device (per spec: single-purpose flasher).
- Do not reproduce Konnected's logo, Typekit font, or Apple's official "Works with Apple Home" badge artwork — all branding/badge art must be original (per spec: original design, same layout feel).
- ESP Web Tools JS must be self-hosted from `espweb/site/vendor/esp-web-tools/`, not loaded from unpkg at runtime (per spec: host the JS).
- The firmware `.bin` must be fetchable same-origin with the page — GitHub Releases assets lack CORS headers (verified via `curl`), so the manifest must never point directly at a `github.com`/`githubusercontent.com` URL.
- The existing GitHub Releases publishing flow in `.github/workflows/build.yml` must keep working unchanged; R2 publishing is additive, not a replacement.
- `scripts/update-espwebtools-manifest.rb` is unrelated legacy tooling — do not modify or wire it in.

---

### Task 1: Vendor ESP Web Tools

**Files:**
- Create: `espweb/site/vendor/esp-web-tools/` (all files from the package's `dist/web/` output)
- Create: `espweb/site/vendor/esp-web-tools/LICENSE`
- Create: `espweb/site/vendor/esp-web-tools/SOURCE.md`

**Interfaces:**
- Produces: `espweb/site/vendor/esp-web-tools/install-button.js` — the ES module later tasks load via `<script type="module" src="vendor/esp-web-tools/install-button.js">`. It registers the `<esp-web-install-button>` custom element with slots `unsupported`, `not-allowed`, `activate` (unused — default button is kept), and honors the CSS custom properties `--esp-tools-button-color`, `--esp-tools-button-text-color`, `--esp-tools-button-border-radius` on the element or an ancestor.

- [ ] **Step 1: Download and extract the pinned esp-web-tools release**

```bash
mkdir -p /tmp/esp-web-tools-vendor && cd /tmp/esp-web-tools-vendor
npm pack esp-web-tools@10.4.0
tar xzf esp-web-tools-10.4.0.tgz
```

- [ ] **Step 2: Copy the built web bundle and license into the repo**

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit"
mkdir -p espweb/site/vendor/esp-web-tools
cp /tmp/esp-web-tools-vendor/package/dist/web/*.js espweb/site/vendor/esp-web-tools/
cp /tmp/esp-web-tools-vendor/package/LICENSE espweb/site/vendor/esp-web-tools/LICENSE
```

- [ ] **Step 3: Record where the vendored copy came from**

Write `espweb/site/vendor/esp-web-tools/SOURCE.md`:

```markdown
# Vendored ESP Web Tools

Source: https://www.npmjs.com/package/esp-web-tools
Version: 10.4.0
License: Apache-2.0 (see LICENSE in this directory)

This directory is the unmodified contents of that package's `dist/web/`
output, copied in so the site never depends on unpkg.com at runtime.

To update:
1. `npm pack esp-web-tools@<new-version>` and extract it.
2. Replace every file in this directory (except this file) with the new
   package's `dist/web/*` and `LICENSE`.
3. Update the version number above and re-test locally with
   `npm run dev` from `espweb/` before deploying.
```

- [ ] **Step 4: Verify the vendored files are self-contained**

Run: `ls espweb/site/vendor/esp-web-tools/`
Expected: `install-button.js` plus the `esp32*-*.js`, `stub_flasher_*.js`, `install-dialog-*.js`, `styles-*.js`, `index-*.js` chunk files, and `LICENSE` / `SOURCE.md` — 17+ files, none missing (install-button.js dynamically imports the hashed chunk files by relative path, so all of them must stay together in this directory).

- [ ] **Step 5: Commit**

```bash
git add espweb/site/vendor/esp-web-tools
git commit -m "$(cat <<'EOF'
Vendor ESP Web Tools 10.4.0 for the espweb installer

Self-hosted so the flasher page never depends on unpkg.com at runtime.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Static site markup, styles, and original branding assets

**Files:**
- Create: `espweb/site/index.html`
- Create: `espweb/site/style.css`
- Create: `espweb/site/assets/logo.svg`
- Create: `espweb/site/assets/favicon.svg`

**Interfaces:**
- Consumes: `espweb/site/vendor/esp-web-tools/install-button.js` (Task 1), `espweb/site/app.js` (Task 3, referenced but not yet created — that's fine, the `<script>` tag is added now and the file lands in Task 3).
- Produces: DOM ids `firmware-version` (`<p>`), `firmware-error` (`<p>`, starts `hidden`), `install-button` (the `<esp-web-install-button>` element), and the `.faq-question` / `.faq-answer` element pairs — all consumed by `espweb/site/app.js` in Task 3.

- [ ] **Step 1: Write the favicon and logo SVGs**

`espweb/site/assets/favicon.svg`:

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">
  <rect width="32" height="32" rx="7" fill="#2f6feb"/>
  <rect x="7" y="9" width="18" height="3.4" rx="1.7" fill="#fff"/>
  <rect x="7" y="14.3" width="18" height="3.4" rx="1.7" fill="#fff"/>
  <rect x="7" y="19.6" width="18" height="3.4" rx="1.7" fill="#fff"/>
</svg>
```

`espweb/site/assets/logo.svg`:

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 220 48" width="220" height="48" role="img" aria-label="GDO blaQ HomeKit">
  <rect x="0" y="4" width="40" height="40" rx="9" fill="#2f6feb"/>
  <rect x="8" y="13" width="24" height="4" rx="2" fill="#fff"/>
  <rect x="8" y="22" width="24" height="4" rx="2" fill="#fff"/>
  <rect x="8" y="31" width="24" height="4" rx="2" fill="#fff"/>
  <text x="50" y="24" font-family="-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif" font-weight="700" font-size="19" fill="#16181d">GDO blaQ</text>
  <text x="50" y="40" font-family="-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif" font-weight="600" font-size="10.5" letter-spacing="1.2" fill="#5b6270">HOMEKIT WEB INSTALLER</text>
</svg>
```

Both are original artwork (a garage-door glyph made of three bars — not Konnected's logo).

- [ ] **Step 2: Write `espweb/site/index.html`**

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GDO blaQ HomeKit — Web Installer</title>
<meta name="description" content="Flash the native HomeKit firmware to your Konnected GDO blaQ directly from Chrome or Edge, no software install required.">
<link rel="icon" href="assets/favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="style.css">
<script type="module" src="vendor/esp-web-tools/install-button.js"></script>
</head>
<body>
<main class="page">
  <header class="hero">
    <img class="hero-logo" src="assets/logo.svg" alt="GDO blaQ HomeKit" width="220" height="48">
    <h1>Web Installer</h1>
    <p class="hero-sub">Flash the native HomeKit firmware to your Konnected GDO blaQ straight from your browser — no software to install.</p>
  </header>

  <section class="card firmware-card">
    <div class="firmware-card-badges">
      <span class="pill pill-beta">BETA</span>
      <span class="apple-home-badge">
        <svg viewBox="0 0 22 22" class="apple-home-badge-icon" aria-hidden="true">
          <path d="M3 10.5 L11 3 L19 10.5" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M5 9 V19 H17 V9" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
          <rect x="9.5" y="13" width="3" height="6" fill="currentColor"/>
        </svg>
        <span class="apple-home-badge-text"><small>Works with</small><strong>Apple Home</strong></span>
      </span>
    </div>

    <h2>GDO blaQ HomeKit firmware</h2>
    <p class="firmware-version" id="firmware-version">Checking latest version…</p>

    <ul class="caveats">
      <li>Not certified by Apple.</li>
      <li>Requires an Apple Home hub (HomePod or Apple TV) running the latest software.</li>
      <li>Replaces the device's current firmware entirely, including a stock ESPHome build.</li>
    </ul>

    <div class="install-row">
      <esp-web-install-button id="install-button" manifest="/firmware/manifest.json">
        <span slot="unsupported" class="unsupported-note">
          <strong>Unsupported browser.</strong> Use <a href="https://www.google.com/chrome/" target="_blank" rel="noopener">Google Chrome</a> or <a href="https://www.microsoft.com/edge" target="_blank" rel="noopener">Microsoft Edge</a> to flash over USB.
        </span>
        <span slot="not-allowed" class="unsupported-note">
          <strong>Web Serial is blocked.</strong> This page needs to be loaded over HTTPS with Web Serial permitted.
        </span>
      </esp-web-install-button>
      <p id="firmware-error" class="firmware-error" hidden>
        Firmware is temporarily unavailable here. Flash manually from the
        <a href="https://github.com/atnorman-us/gdo-blaq-homekit/releases" target="_blank" rel="noopener">GitHub Releases page</a> instead.
      </p>
    </div>
  </section>

  <section class="card steps-card">
    <h2>After flashing: provision WiFi and add to HomeKit</h2>
    <ol>
      <li>Connect to the device's <code>konnected-blaq-hk</code> WiFi access point.</li>
      <li>Open <code>http://192.168.4.1</code>, enter your WiFi credentials, and select <strong>Write and Reboot</strong>.</li>
      <li>Once the device joins your network, open Apple's Home app, select <strong>Add Accessory</strong>, then <strong>More Options</strong>.</li>
      <li>Select the accessory and enter setup code <code>251-02-023</code>.</li>
    </ol>
  </section>

  <section class="faq">
    <h2>Troubleshooting / FAQ</h2>
    <div class="faq-item">
      <button class="faq-question" aria-expanded="false">Which browsers are supported?</button>
      <div class="faq-answer" hidden>
        <p>Use <strong>Google Chrome</strong> or <strong>Microsoft Edge</strong>. Safari and Firefox don't support the Web Serial API this tool needs.</p>
      </div>
    </div>
    <div class="faq-item">
      <button class="faq-question" aria-expanded="false">Do I need to install USB drivers?</button>
      <div class="faq-answer" hidden>
        <p>No. The GDO blaQ's ESP32-S3 uses native USB and needs no drivers on Windows, Mac, or Linux.</p>
      </div>
    </div>
    <div class="faq-item">
      <button class="faq-question" aria-expanded="false">My device isn't detected when I click Connect</button>
      <div class="faq-answer" hidden>
        <p>Make sure you're using a data-quality USB-C cable — some cables are charging-only and don't carry data. Try a different cable or port if the device doesn't appear in the port list.</p>
      </div>
    </div>
    <div class="faq-item">
      <button class="faq-question" aria-expanded="false">Which serial port should I select?</button>
      <div class="faq-answer" hidden>
        <p><strong>Windows:</strong> look for a COM port (e.g. COM3, COM4).<br>
        <strong>Mac:</strong> look for a port containing "usbserial" or "usbmodem" in its name.</p>
      </div>
    </div>
    <div class="faq-item">
      <button class="faq-question" aria-expanded="false">Where do I get more help?</button>
      <div class="faq-answer" hidden>
        <p>Open an issue on the <a href="https://github.com/atnorman-us/gdo-blaq-homekit" target="_blank" rel="noopener">GitHub repository</a>, or read the full <a href="https://github.com/atnorman-us/gdo-blaq-homekit#readme" target="_blank" rel="noopener">README</a>.</p>
      </div>
    </div>
  </section>

  <footer class="footer">
    <p>
      <a href="https://github.com/atnorman-us/gdo-blaq-homekit" target="_blank" rel="noopener">GDO blaQ HomeKit firmware</a>
      is free software, licensed under the
      <a href="https://github.com/atnorman-us/gdo-blaq-homekit/blob/main/LICENSE" target="_blank" rel="noopener">GNU GPLv3</a>.
    </p>
    <p><a href="https://esphome.github.io/esp-web-tools/" target="_blank" rel="noopener">Powered by ESP Web Tools</a></p>
  </footer>
</main>
<script src="app.js"></script>
</body>
</html>
```

- [ ] **Step 3: Write `espweb/site/style.css`**

```css
:root {
  --color-accent: #2f6feb;
  --color-accent-dark: #1f4fd1;
  --color-bg: #ffffff;
  --color-bg-alt: #f6f7f9;
  --color-text: #16181d;
  --color-text-muted: #5b6270;
  --color-border: #e3e5ea;
  --color-beta-bg: #fff3d6;
  --color-beta-text: #8a5a00;

  --esp-tools-button-color: var(--color-accent);
  --esp-tools-button-text-color: #ffffff;
  --esp-tools-button-border-radius: 8px;
}

* { box-sizing: border-box; }

body {
  margin: 0;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  color: var(--color-text);
  background: var(--color-bg-alt);
}

.page {
  max-width: 720px;
  margin: 0 auto;
  padding: 48px 20px 64px;
}

.hero { text-align: center; margin-bottom: 32px; }
.hero-logo { display: block; margin: 0 auto 16px; }
.hero h1 { font-size: 28px; font-weight: 700; margin: 0 0 8px; }
.hero-sub { color: var(--color-text-muted); font-size: 16px; line-height: 1.5; margin: 0 auto; max-width: 520px; }

.card {
  background: var(--color-bg);
  border: 1px solid var(--color-border);
  border-radius: 12px;
  padding: 28px;
  margin-bottom: 20px;
}

.firmware-card-badges { display: flex; align-items: center; gap: 10px; margin-bottom: 16px; flex-wrap: wrap; }

.pill {
  display: inline-block;
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.06em;
  padding: 4px 10px;
  border-radius: 999px;
  text-transform: uppercase;
}
.pill-beta { background: var(--color-beta-bg); color: var(--color-beta-text); }

.apple-home-badge {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  border: 1px solid var(--color-border);
  border-radius: 999px;
  padding: 4px 12px 4px 8px;
  color: var(--color-text);
}
.apple-home-badge-icon { width: 18px; height: 18px; }
.apple-home-badge-text { display: flex; flex-direction: column; line-height: 1.1; font-size: 12px; }
.apple-home-badge-text small { color: var(--color-text-muted); font-size: 10px; }

.firmware-card h2 { margin: 0 0 4px; font-size: 18px; }
.firmware-version { color: var(--color-text-muted); font-size: 14px; margin: 0 0 16px; }

.caveats { margin: 0 0 24px; padding-left: 20px; color: var(--color-text-muted); font-size: 14px; line-height: 1.6; }

.install-row { text-align: center; }
esp-web-install-button { display: inline-block; }

.unsupported-note, .firmware-error {
  display: block;
  margin-top: 12px;
  font-size: 13px;
  color: var(--color-text-muted);
}
.firmware-error { color: #b3261e; }

.steps-card ol { padding-left: 20px; line-height: 1.7; }
.steps-card code {
  background: var(--color-bg-alt);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  padding: 1px 6px;
  font-size: 13px;
}

.faq { margin-top: 32px; }
.faq h2 { font-size: 18px; margin-bottom: 8px; }
.faq-item { border-bottom: 1px solid var(--color-border); }
.faq-question {
  width: 100%;
  text-align: left;
  background: none;
  border: none;
  padding: 14px 0;
  font-size: 15px;
  font-weight: 600;
  color: var(--color-text);
  cursor: pointer;
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.faq-question::after { content: '+'; font-weight: 300; font-size: 20px; color: var(--color-text-muted); }
.faq-question[aria-expanded="true"]::after { content: '\2212'; }
.faq-answer { padding: 0 0 16px; color: var(--color-text-muted); font-size: 14px; line-height: 1.6; }
.faq-answer p { margin: 0; }

.footer { margin-top: 40px; text-align: center; font-size: 12px; color: var(--color-text-muted); }
.footer a { color: var(--color-text-muted); }
.footer p { margin: 4px 0; }

a { color: var(--color-accent); }

@media (max-width: 480px) {
  .page { padding: 32px 16px 48px; }
  .card { padding: 20px; }
}
```

- [ ] **Step 4: Commit**

```bash
git add espweb/site/index.html espweb/site/style.css espweb/site/assets
git commit -m "$(cat <<'EOF'
Add espweb installer page markup, styles, and original branding

Single-purpose ESP Web Tools flasher page for the HomeKit firmware,
styled after install.konnected.io's layout with original logo/badge
artwork (no Konnected or Apple trademarked assets).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Page interactivity (FAQ accordion, manifest status)

**Files:**
- Create: `espweb/site/app.js`

**Interfaces:**
- Consumes: DOM ids `firmware-version`, `firmware-error`, `install-button`, and `.faq-question`/`.faq-answer` pairs from `espweb/site/index.html` (Task 2). Fetches `GET /firmware/manifest.json`, expecting `{ "version": string, ... }` on success (shape produced by Task 4's Pages Function) or a non-2xx status on failure.
- Produces: nothing consumed by later tasks — this is a leaf.

- [ ] **Step 1: Write `espweb/site/app.js`**

```javascript
document.querySelectorAll(".faq-question").forEach((button) => {
  button.addEventListener("click", () => {
    const answer = button.nextElementSibling;
    const expanded = button.getAttribute("aria-expanded") === "true";
    button.setAttribute("aria-expanded", String(!expanded));
    answer.hidden = expanded;
  });
});

const versionEl = document.getElementById("firmware-version");
const errorEl = document.getElementById("firmware-error");
const installButton = document.getElementById("install-button");

fetch("/firmware/manifest.json")
  .then((response) => {
    if (!response.ok) {
      throw new Error(`manifest request failed: ${response.status}`);
    }
    return response.json();
  })
  .then((manifest) => {
    versionEl.textContent = `Version ${manifest.version}`;
  })
  .catch(() => {
    versionEl.textContent = "Version unavailable";
    errorEl.hidden = false;
    installButton.hidden = true;
  });
```

- [ ] **Step 2: Verify the FAQ accordion logic in isolation with Node**

This is plain DOM-manipulation code with no build step, so verify it by inspection plus the browser check in Task 6/9 rather than a headless unit test — there's no existing JS test harness in this repo to extend for one function this small (YAGNI). Confirm by reading the file that:
- Every `.faq-question` toggles its own `aria-expanded` and its immediate next sibling's `hidden`.
- The manifest fetch failure path sets `errorEl.hidden = false` and hides the install button, matching the spec's error-handling section.

- [ ] **Step 3: Commit**

```bash
git add espweb/site/app.js
git commit -m "$(cat <<'EOF'
Add espweb page interactivity: FAQ accordion and manifest status

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Pages Function — manifest + firmware proxy from R2

**Files:**
- Create: `espweb/functions/firmware/[[path]].js`

**Interfaces:**
- Consumes: an R2 binding named `FIRMWARE` (configured in Task 5's `wrangler.jsonc`), and an object at key `current.json` in that bucket shaped `{"version": string, "file": string}` (written by Task 8's CI step).
- Produces: `GET /firmware/manifest.json` → `200` with body `{"name": "GDO blaQ HomeKit firmware", "version": "<version>", "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": "/firmware/<file>", "offset": 0}]}]}`, or `503` with `{"error": "firmware manifest unavailable"}` if `current.json` is missing/unparseable. `GET /firmware/<file>` → `200` streaming the matching R2 object with its content-type/etag, or `404` if absent. Consumed by `espweb/site/app.js` (Task 3) and by ESP Web Tools itself (Task 2's `<esp-web-install-button manifest="/firmware/manifest.json">`).

- [ ] **Step 1: Write `espweb/functions/firmware/[[path]].js`**

```javascript
const MANIFEST_NAME = "GDO blaQ HomeKit firmware";

export async function onRequestGet(context) {
  const { env, params } = context;
  const segments = Array.isArray(params.path)
    ? params.path
    : params.path
      ? [params.path]
      : [];
  const requestedPath = segments.join("/");

  if (requestedPath === "" || requestedPath === "manifest.json") {
    return handleManifest(env);
  }

  return handleFirmwareFile(env, requestedPath);
}

async function readCurrent(env) {
  const pointer = await env.FIRMWARE.get("current.json");
  if (!pointer) return null;
  try {
    return JSON.parse(await pointer.text());
  } catch {
    return null;
  }
}

async function handleManifest(env) {
  const current = await readCurrent(env);
  if (!current || !current.version || !current.file) {
    return Response.json(
      { error: "firmware manifest unavailable" },
      { status: 503, headers: { "cache-control": "no-store" } },
    );
  }

  const manifest = {
    name: MANIFEST_NAME,
    version: current.version,
    builds: [
      {
        chipFamily: "ESP32-S3",
        parts: [{ path: `/firmware/${current.file}`, offset: 0 }],
      },
    ],
  };

  return Response.json(manifest, { headers: { "cache-control": "no-store" } });
}

async function handleFirmwareFile(env, key) {
  if (!key || key.includes("..")) {
    return new Response("Not found", { status: 404 });
  }

  const object = await env.FIRMWARE.get(key);
  if (!object) {
    return new Response("Not found", { status: 404 });
  }

  const headers = new Headers();
  object.writeHttpMetadata(headers);
  headers.set("etag", object.httpEtag);
  headers.set("cache-control", "public, max-age=3600");
  if (!headers.has("content-type")) {
    headers.set("content-type", "application/octet-stream");
  }

  return new Response(object.body, { headers });
}
```

- [ ] **Step 2: Note the verification path**

This function needs a real R2 binding to run, which only exists once Task 5's `wrangler.jsonc` and Task 6's provisioned bucket exist. It's verified end-to-end in Task 6 (`wrangler pages dev` + `curl`), not here in isolation — a mocked-binding unit test would add a Vitest + `@cloudflare/vitest-pool-workers` harness for one 60-line function, which is more scaffolding than the function warrants (YAGNI). Leave this task's code as the deliverable; Task 6 is its test cycle.

- [ ] **Step 3: Commit**

```bash
git add espweb/functions
git commit -m "$(cat <<'EOF'
Add Pages Function proxying firmware manifest + binary from R2

Serves /firmware/manifest.json and /firmware/<file> same-origin with
the site, so ESP Web Tools never makes a cross-origin request for
firmware (GitHub Releases assets lack the CORS headers it needs).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Wrangler project config and package scaffolding

**Files:**
- Create: `espweb/wrangler.jsonc`
- Create: `espweb/package.json`
- Create: `espweb/.gitignore`

**Interfaces:**
- Produces: the `FIRMWARE` R2 binding name that Task 4's function reads (`env.FIRMWARE`), and the `npm run dev` / `npm run deploy` scripts Task 6 and Task 7 use.

- [ ] **Step 1: Check the current wrangler major version before pinning it**

```bash
npm view wrangler version
```

Use whatever major version this prints in Step 2 below (e.g. if it prints `4.42.0`, pin `"wrangler": "^4.0.0"`).

- [ ] **Step 2: Write `espweb/package.json`**

```json
{
  "name": "gdo-blaq-espweb",
  "private": true,
  "version": "1.0.0",
  "scripts": {
    "dev": "wrangler pages dev",
    "deploy": "wrangler pages deploy"
  },
  "devDependencies": {
    "wrangler": "^4.0.0"
  }
}
```

(Replace `^4.0.0` with the major version found in Step 1 if different.)

- [ ] **Step 3: Write `espweb/wrangler.jsonc`**

```jsonc
{
  "$schema": "node_modules/wrangler/config-schema.json",
  "name": "gdo-blaq-espweb",
  "pages_build_output_dir": "site",
  "compatibility_date": "2026-09-16",
  "r2_buckets": [
    { "binding": "FIRMWARE", "bucket_name": "gdo-blaq-firmware" },
  ],
}
```

- [ ] **Step 4: Write `espweb/.gitignore`**

```
node_modules/
.wrangler/
.dev.vars
```

- [ ] **Step 5: Install dependencies and confirm wrangler runs**

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit/espweb"
npm install
npx wrangler --version
```

Expected: prints an installed wrangler version with no errors.

- [ ] **Step 6: Commit**

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit"
git add espweb/wrangler.jsonc espweb/package.json espweb/package-lock.json espweb/.gitignore
git commit -m "$(cat <<'EOF'
Add wrangler/package scaffolding for the espweb Pages project

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Provision Cloudflare resources and verify locally

**Files:** none (infrastructure + manual verification only)

**Interfaces:**
- Consumes: `espweb/wrangler.jsonc` (Task 5), `espweb/functions/firmware/[[path]].js` (Task 4), `espweb/site/*` (Tasks 1-3).
- Produces: a live R2 bucket named `gdo-blaq-firmware` and a live Cloudflare Pages project named `gdo-blaq-espweb`, both required by Task 7 (deploy) and Task 8 (CI).

- [ ] **Step 1: Authenticate wrangler (user action)**

Ask the user to run this themselves in a terminal (it opens a browser for OAuth):

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit/espweb"
npx wrangler login
```

Wait for them to confirm it succeeded before continuing.

- [ ] **Step 2: Confirm the authenticated account**

```bash
npx wrangler whoami
```

Expected: prints the Cloudflare account email/ID that owns the `terazone.com` zone. Confirm with the user this is the right account before creating billable resources in it.

- [ ] **Step 3: Create the R2 bucket**

```bash
npx wrangler r2 bucket create gdo-blaq-firmware
```

Expected: success message with the bucket name.

- [ ] **Step 4: Seed a `current.json` pointer so local dev has something to serve**

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit/espweb"
echo '{"version":"1.3.1.9","file":"konnected-gdo-blaq-homekit-1.3.1.9.bin"}' > /tmp/current.json
npx wrangler r2 object put gdo-blaq-firmware/current.json --file /tmp/current.json --content-type application/json --remote
curl -sL -o /tmp/konnected-gdo-blaq-homekit-1.3.1.9.bin \
  https://github.com/atnorman-us/gdo-blaq-homekit/releases/download/1.3.1.9/konnected-gdo-blaq-homekit-1.3.1.9.bin
npx wrangler r2 object put gdo-blaq-firmware/konnected-gdo-blaq-homekit-1.3.1.9.bin \
  --file /tmp/konnected-gdo-blaq-homekit-1.3.1.9.bin --content-type application/octet-stream --remote
```

If `--remote` is rejected by the installed wrangler version, drop the flag — check `npx wrangler r2 object put --help` for the current flag name for "target the real bucket, not local simulation" and use that instead.

- [ ] **Step 5: Create the Pages project**

```bash
npx wrangler pages project create gdo-blaq-espweb --production-branch=main
```

Expected: success message with the project's `*.pages.dev` URL.

- [ ] **Step 6: Run the site locally against the real R2 bucket**

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit/espweb"
npx wrangler pages dev --r2=FIRMWARE=gdo-blaq-firmware --remote
```

- [ ] **Step 7: Verify the manifest endpoint**

In a second terminal:

```bash
curl -s http://localhost:8788/firmware/manifest.json
```

Expected: `{"name":"GDO blaQ HomeKit firmware","version":"1.3.1.9","builds":[{"chipFamily":"ESP32-S3","parts":[{"path":"/firmware/konnected-gdo-blaq-homekit-1.3.1.9.bin","offset":0}]}]}`

- [ ] **Step 8: Verify the firmware proxy streams the right bytes**

```bash
curl -s -o /tmp/downloaded.bin http://localhost:8788/firmware/konnected-gdo-blaq-homekit-1.3.1.9.bin
diff /tmp/konnected-gdo-blaq-homekit-1.3.1.9.bin /tmp/downloaded.bin
```

Expected: `diff` prints nothing (files identical).

- [ ] **Step 9: Verify the page itself loads and the FAQ/install button render**

Open `http://localhost:8788/` in Chrome (use the built-in browser tool: `preview_start` with `url: "http://localhost:8788/"`, or the user's own Chrome). Confirm: the hero/logo/badge render, "Version 1.3.1.9" appears under the firmware name, the Connect button is visible, and clicking a FAQ question expands its answer.

- [ ] **Step 10: Stop the dev server**

Stop the `wrangler pages dev` process (Ctrl-C, or `preview_stop` if started via the browser tool's dev-server helper).

No commit — this task only provisions cloud resources and verifies them; there's no new repo content.

---

### Task 7: Deploy to Cloudflare Pages and attach the custom domain

**Files:** none (deployment only)

**Interfaces:**
- Consumes: everything from Tasks 1-6.
- Produces: `espweb.terazone.com` serving the live site.

- [ ] **Step 1: Deploy**

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit/espweb"
npx wrangler pages deploy site --project-name=gdo-blaq-espweb
```

Expected: a deployment URL like `https://<hash>.gdo-blaq-espweb.pages.dev`.

- [ ] **Step 2: Verify the deployed manifest and firmware proxy**

```bash
curl -s https://gdo-blaq-espweb.pages.dev/firmware/manifest.json
```

Expected: same manifest JSON as Task 6 Step 7.

- [ ] **Step 3: Get the account ID for the custom-domain API call**

```bash
npx wrangler whoami
```

Note the Account ID printed.

- [ ] **Step 4: Create an API token for the domain-attach call (user action)**

Ask the user to create a Cloudflare API token at `https://dash.cloudflare.com/profile/api-tokens` with the **"Cloudflare Pages: Edit"** permission for their account, and give it to you to use for this one command only (it does not need to be saved anywhere — Task 8 will need a separately-scoped token for CI, created later).

- [ ] **Step 5: Attach `espweb.terazone.com` as a custom domain on the Pages project**

```bash
curl -s -X POST \
  "https://api.cloudflare.com/client/v4/accounts/<ACCOUNT_ID>/pages/projects/gdo-blaq-espweb/domains" \
  -H "Authorization: Bearer <API_TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{"name": "espweb.terazone.com"}'
```

Expected: `"success": true` in the response, and `"status"` on the created domain is `"initializing"` or `"pending"`. Since `terazone.com` is already on Cloudflare's nameservers in this account (confirmed via `dig` during brainstorming), the necessary DNS record is created automatically — no manual DNS step should be needed.

- [ ] **Step 6: Confirm the domain becomes active**

```bash
curl -s "https://api.cloudflare.com/client/v4/accounts/<ACCOUNT_ID>/pages/projects/gdo-blaq-espweb/domains" \
  -H "Authorization: Bearer <API_TOKEN>"
```

Poll (a minute or two apart) until `"status": "active"` for `espweb.terazone.com`.

- [ ] **Step 7: Revoke the temporary token**

This token has full Pages-edit access to the account and is no longer needed after Step 6. Ask the user to revoke it at `https://dash.cloudflare.com/profile/api-tokens` (or run `npx wrangler` isn't applicable here — it's a dashboard action since the token wasn't created via CLI).

- [ ] **Step 8: Verify the live domain**

```bash
curl -s https://espweb.terazone.com/firmware/manifest.json
```

Expected: same manifest JSON as Task 6 Step 7. Then open `https://espweb.terazone.com/` in a browser and confirm the page renders correctly (same check as Task 6 Step 9).

No commit — deployment only.

---

### Task 8: CI — publish new releases to R2 automatically

**Files:**
- Modify: `.github/workflows/build.yml` (the `release` job)

**Interfaces:**
- Consumes: `RELEASE_TAG` and `release-assets/konnected-gdo-blaq-homekit-<tag>.bin`, both already produced by the existing `release` job steps in this file.
- Produces: an updated `current.json` and a new firmware object in the `gdo-blaq-firmware` R2 bucket on every future GitHub Release — read by Task 4's Pages Function.

- [ ] **Step 1: Read the current release job**

```bash
cat "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit/.github/workflows/build.yml"
```

Confirm the `release` job's last step is still `Publish release firmware` ending in `gh release upload ...`, matching what brainstorming found. If it has changed, adjust the insertion point in Step 3 below accordingly.

- [ ] **Step 2: Ask the user to create the CI-scoped Cloudflare API token and add repo secrets (user action)**

Ask the user to:
1. Create a Cloudflare API token at `https://dash.cloudflare.com/profile/api-tokens` scoped to R2 write access only (labeled something like "Workers R2 Storage: Edit" in the token-creation UI — the exact label may vary by dashboard version; look for the R2-specific edit permission rather than a full-account Edit token). This should be a different, narrower token than the Pages-edit one from Task 7, which gets revoked after that task.
2. Add it as a repository secret, plus the account ID, either by running these themselves:
   ```bash
   gh secret set CLOUDFLARE_API_TOKEN --repo atnorman-us/gdo-blaq-homekit
   gh secret set CLOUDFLARE_ACCOUNT_ID --repo atnorman-us/gdo-blaq-homekit
   ```
   or by pasting the values to you so you can run those two commands on their behalf.

Do not proceed to Step 3 until both secrets exist (`gh secret list --repo atnorman-us/gdo-blaq-homekit` should show both).

- [ ] **Step 3: Add the R2 publish step to the `release` job**

Modify `.github/workflows/build.yml`: in the `release` job, add a `Set up Node.js` step before the existing `Publish release firmware` step, and a new `Publish firmware to Cloudflare R2` step after it:

```yaml
    - name: Set up Node.js
      uses: actions/setup-node@v4
      with:
        node-version: 20

    - name: Publish release firmware
      env:
        GH_TOKEN: ${{ github.token }}
        GH_REPO: ${{ github.repository }}
        RELEASE_TAG: ${{ github.event.release.tag_name }}
      run: |
        mkdir -p release-assets
        cp release-parts/gdo-blaq-homekit.bin "release-assets/gdo-blaq-homekit-${RELEASE_TAG}-ota.bin"
        cp release-combined/konnected-gdo-blaq-homekit.bin "release-assets/konnected-gdo-blaq-homekit-${RELEASE_TAG}.bin"
        (cd release-assets && sha256sum *.bin > SHA256SUMS.txt)
        gh release upload "$RELEASE_TAG" release-assets/* --clobber

    - name: Publish firmware to Cloudflare R2
      env:
        CLOUDFLARE_API_TOKEN: ${{ secrets.CLOUDFLARE_API_TOKEN }}
        CLOUDFLARE_ACCOUNT_ID: ${{ secrets.CLOUDFLARE_ACCOUNT_ID }}
        RELEASE_TAG: ${{ github.event.release.tag_name }}
      run: |
        FIRMWARE_FILE="konnected-gdo-blaq-homekit-${RELEASE_TAG}.bin"
        npx --yes wrangler@4 r2 object put "gdo-blaq-firmware/${FIRMWARE_FILE}" \
          --file "release-assets/${FIRMWARE_FILE}" \
          --content-type application/octet-stream \
          --remote
        echo "{\"version\":\"${RELEASE_TAG}\",\"file\":\"${FIRMWARE_FILE}\"}" > /tmp/current.json
        npx --yes wrangler@4 r2 object put "gdo-blaq-firmware/current.json" \
          --file /tmp/current.json \
          --content-type application/json \
          --remote
```

(`wrangler@4` should match whichever major version Task 5 Step 1 found — keep them in sync.)

The existing `Publish release firmware` step's body is unchanged; only its position relative to the new steps matters.

- [ ] **Step 4: Validate the YAML**

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit"
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/build.yml'))" && echo "valid YAML"
```

Expected: `valid YAML` with no exception.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/build.yml
git commit -m "$(cat <<'EOF'
Publish release firmware to Cloudflare R2 for the espweb installer

Every GitHub Release now also pushes the combined image and an
updated current.json pointer to the gdo-blaq-firmware R2 bucket, so
espweb.terazone.com always serves the latest firmware without a
separate site redeploy.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 6: Note the manual verification that has to wait for a real release**

This step can't be fully exercised until the next GitHub Release is published (that's the trigger). Tell the user: the next time they publish a release, check the Actions run for the new "Publish firmware to Cloudflare R2" step, then confirm `https://espweb.terazone.com/firmware/manifest.json` reports the new version.

---

### Task 9: End-to-end verification on the live site

**Files:** none (verification only)

**Interfaces:** none — this is the final check that every earlier task's pieces work together for a real user.

- [ ] **Step 1: Load the live site in Chrome**

```
https://espweb.terazone.com/
```

Confirm: original logo/hero render, "BETA" pill and "Works with Apple Home" badge are visible, the firmware version line reads "Version 1.3.1.9" (or whatever the current release tag is), and no `firmware-error` fallback message is showing.

- [ ] **Step 2: Confirm the Connect flow starts correctly**

Click **Connect**. Confirm the browser's native serial-port picker dialog opens (this confirms Web Serial + the manifest fetch both worked — actually completing a flash requires physical GDO blaQ hardware attached over USB, which this session can't do; note to the user that they should do one real end-to-end flash themselves when convenient).

- [ ] **Step 3: Confirm the FAQ accordion and footer links work**

Click each FAQ question and confirm it expands/collapses. Confirm the footer's GitHub and ESP Web Tools links open the right pages.

- [ ] **Step 4: Confirm the fallback path**

Temporarily break the manifest on purpose to check the error path renders correctly:

```bash
cd "/Users/anorman/Library/CloudStorage/OneDrive-Personal/projects/gdo-blaq-homekit/espweb"
npx wrangler r2 object delete gdo-blaq-firmware/current.json --remote
```

Reload `https://espweb.terazone.com/`. Confirm: "Version unavailable" shows, the firmware-error message pointing at GitHub Releases appears, and the Connect button is hidden.

- [ ] **Step 5: Restore the pointer**

```bash
npx wrangler r2 object put gdo-blaq-firmware/current.json --file /tmp/current.json --content-type application/json --remote
```

Reload the page again and confirm it's back to normal (Version 1.3.1.9, Connect button visible, no error message).

- [ ] **Step 6: Report status to the user**

Summarize what's live: the URL, that firmware auto-updates on future GitHub Releases via Task 8's CI step, and that a real hardware flash test is the one thing this session couldn't verify itself.
