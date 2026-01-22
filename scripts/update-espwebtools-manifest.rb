require 'uri'
require 'json'

manifest = {
  name: ENV['MANIFEST_NAME'],
  version: ENV['MANIFEST_VERSION'],
  builds: [
    chipFamily: 'ESP32-S3',
    parts: [
      { path: ENV['ESP32_S3_IMAGE_URI'], offset: 0 }
    ]
  ]
}

json = JSON.dump(manifest)
puts json
File.write("install-konnected/manifests/#{ENV['MANIFEST_FNAME']}", json)