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
