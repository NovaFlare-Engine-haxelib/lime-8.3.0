# NovaFlare compatibility

Keeps NovaFlare's SDL 2.30.12 and ordinary hxcpp backend. Adds the CNE/Origin CFFI primitive union, callable adapters, Future/Promise methods, audio decoder/source APIs, and both file-drop callback shapes.

Prebuilt compatibility binaries cover Windows x64 and Android ARMv7/ARM64/x86_64, release and debug. Other targets must rebuild Lime before using the added native primitives.

OGG/Vorbis, WAV PCM, and BMP adapters are implemented. Optional FLAC/MP3/Opus decoders and GIF/SVG/WebP animation entry points return an unsupported result. Stream-compatible buffers currently decode into memory. AudioSource keeps NF's original four-argument constructor (buffer, offset, length, loops), including integer offsets/lengths. New per-channel peak values fall back to zero when unavailable.

Upstream licenses and contributor notices are preserved.
