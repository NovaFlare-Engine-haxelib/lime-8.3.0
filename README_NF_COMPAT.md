# NovaFlare additive compatibility interfaces

Restores NF AudioManager init/resume/suspend/shutdown, Future/Promise implementations and the original SDL2 event pump. Newly declared precise input/drop/device events are optional compatibility stubs and are not automatically wired into legacy update paths. Existing native primitive implementations retain their ABI; differing new dialog arities route to separate adapters. The original audio playback backend is retained; optional new audio effect routing is unsupported. SDL2 and ordinary hxcpp are retained.

The earlier broad integration changed existing behavior and is superseded by this repair. Compatibility additions must preserve existing NF calls, defaults and update/render/audio paths. Unsupported additions may return a neutral result instead of replacing a legacy implementation.

Windows x64 and Android ARMv7/ARM64/x86_64 native Lime binaries have been rebuilt. The full game targets Windows x64 and Android ARM64. Visual gameplay acceptance is performed manually by the project owner.
