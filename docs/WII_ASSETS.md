# The Wii version's assets

The Wii release of Echoes of Time (disc ID `RFFEGD` for the USA release) runs the same engine as the DS game, recompiled for
the Wii's PowerPC. This page records what its disc holds, how it compares with the DS ROM, and what the program does with
it. Nothing from the disc is in this repository (see [LEGAL.md](LEGAL.md)); it is all read from the player's own dump.

## What the program does: Wii cutscenes

The Wii has the same 38 cutscenes (`movie/movie001.mods` ... `movie038.mods`, the same numbering as the DS), as 384x192 video
instead of the DS's 256x128. If the player sets them up (Settings, Game files, "Wii cutscenes"), the game keeps playing its
own movie, with its sound and timing, and the program draws the Wii picture for the same frame over the DS's movie area.

Pieces:

- `common/src/wii_disc.cpp`, `aes.cpp`: reads a WBFS or ISO dump, decrypts the game partition, lists and extracts files. The
  disc is AES-128-CBC encrypted. The title key is in the disc's ticket, encrypted with the Wii's *common key*, which is not
  in this repository: the player types it in (Settings, Game files, "Wii common key"), and it is kept in memory only. A wrong
  key is told from a right one because the decrypted disc header must read as the game's id. The test (`tests/test_wii_disc.cpp`)
  builds a small disc of its own, with made-up keys, in both layouts.
- `game/src/ui/wii_setup.cpp`: the setup window. Copies `movie/movieNNN.mods` to `<data>/wii/movie/movieNNN.thp` (about 1.9 GB;
  these files are THP, a Nintendo container of JPEG frames, whatever their name says).
- `game/src/thp.cpp`: reads THP and decodes a frame with `stb_image`. The Wii files leave out the JPEG byte stuffing (a zero
  after every `FF`), which the decoder needs, so it is put back first.
- `game/src/wii_cutscenes.cpp`: opens the file for the movie that is playing and picks the frame.
- `game/src/video.cpp`: draws the picture over the top screen's movie area, in whatever layout is in use.
- Hooks (`game/config/<release>/hooks.txt`): `cfie_on_movie_open/frame/close` read the movie id and frame count from the game's
  movie object, so the frontend knows which movie is playing and where it is.

### Alignment

When the hook reports frame *k*, the DS is showing the picture of frame *k* - 2 (its decoder runs two frames ahead of what is
displayed). The Wii picture for frame *k* is therefore Wii frame *k* - 2, clamped to the movie's length. This was measured by
playing every one of the 38 cutscenes on the DS side, dumping the pictures, and matching them to the Wii frames (downscaled):
the offset is -2 from the first frame to the last in all of them. Fades to and from black are part of both videos.

The Wii cuts of 24 movies are longer than the DS's, by about 30 or 60 frames (2 to 4 seconds). The extra frames are at the
end and are not shown; the DS movie decides how long the cutscene lasts.

### Not done

- The sound stays the DS's. The Wii's music and effects are a different sound engine (see below).
- `caster.thp` (the credits, 640x480 with stereo sound) is Wii-only and unused.
- The subtitles and text boxes some cutscenes show are drawn by the DS game, not part of the video.

## Comparison of the two versions' files

| What | DS ROM | Wii disc |
|---|---|---|
| `coolres.rsb` (models, textures, animations, scripts) | 52.8 MB, little-endian | 29.3 MB, big-endian, plus `coolres0-4.rsb` (24 MB, one per language) and `coolresn.rsb` (13 MB, `TEX ` chunks). The index tables match the DS's word for word (2031 of 2048 header words), so the resource ids are the same: the same engine, with textures moved to `coolresn` and text to `coolresN`. |
| Movies | 37 `movieNNN.mods`, Mobiclip 256x128, 15 fps | 38 `movieNNN.mods` that are THP (JPEG) video, 384x192, 15 fps, no sound; 40 to 130 times the bytes |
| Music | `music.sdat` (SSEQ sequences and banks) | `music.sdat` is an RSAR (RSEQ and banks, DSP-ADPCM) |
| Sound effects | `se.sdat`, 18.8 MB, 2363 named effects | `se.sdat` (RSAR) with 2184 numbered streams, and `se.brstm`, 12.9 MB: smaller, and no id mapping is known |
| Textures, models | DS GX commands, paletted and compressed textures | `TEX ` chunks and GX display lists |

The Wii's total size is only about 25% more than the DS's, and the tables are identical, which suggests mostly the same assets
in a different packing. Whether the Wii's textures and models are actually sharper is not established; that is the texture
and model survey below.

### Texture and model survey

Not done yet. The plan: pair the resources of the two `coolres` files by id, and compare texture dimensions and formats and
model vertex counts. A texture replacement would be keyed by the DS texture's content hash in the OpenGL renderer; better
models would need a conversion from the Wii's GX display lists to the DS's geometry stream, which the DS GPU's polygon and
vertex limits restrict.
