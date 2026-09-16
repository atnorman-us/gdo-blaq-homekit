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
