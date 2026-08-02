-- Helper function to add APU transitive dependencies
-- Call this after linking to xenia-apu to ensure all transitive dependencies are included
-- On Linux, final consumers need the SDL audio backend and its SDL3 dependency.
function apu_transitive_deps()
  filter("platforms:Linux-*")
    links({
      "xenia-apu-sdl",  -- Contains SDLAudioDriver used by AudioMediaPlayer
    })
    sdl3_link()
  filter({})
end
