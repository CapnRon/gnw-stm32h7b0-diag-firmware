# Building your own asset blob

This diagnostic firmware plays sound effects and shows a bouncing image, but
this repo does not include any audio or image files. Build your own blob
from your own files with the script in this folder.

## Steps

1. Put your own audio files in one folder. Use `.wav`, `.mp3`, `.flac`, or
   `.ogg`. Only use files you have the right to use.
2. Pick one image file for the bouncing photo demo. Use `.png` or `.jpg`.
3. Install the two tools this script needs:
   ```bash
   apt-get install -y ffmpeg
   pip install pillow
   ```
4. Run the build script from this folder:
   ```bash
   python3 build_assets.py --sounds-dir /path/to/your/sounds --image /path/to/your/image.png
   ```
   This writes two files:
   - `assets/assets.bin`: the combined binary. Flash this to the device.
   - `../Core/Inc/sound_assets.h`: the matching offset table. Rebuild the
     firmware after this file changes.
5. Rebuild the firmware:
   ```bash
   cd ..
   make -j8 GNW_TARGET=zelda EXTFLASH_SIZE_MB=64
   ```
6. Flash the asset blob to external flash:
   ```bash
   gnwmanager erase --location ext
   gnwmanager flash --location ext --file assets/assets.bin
   ```

## Notes

- The script skips any audio file longer than 2.5 seconds by default. This
  keeps each clip small enough for the diagnostic firmware's playback
  buffer. Change this with `--max-seconds`.
- The script converts audio to mono 16-bit PCM at 48kHz by default. These
  values must match the SAI peripheral setup in `Core/Src/main.c`
  (`hsai_BlockA1.Init.MonoStereoMode` and `SND_SAMPLE_RATE_HZ`). Do not
  change one without the other.
- The image gets resized to 140x140 and converted to RGB565 (16-bit color).
  Pixels with low alpha (below 128) become a magenta sentinel color
  (`0xF81F`), which the firmware treats as transparent so the color-bar
  background shows through around the image.
- Neither `assets.bin` nor your source files are tracked by git (see
  `.gitignore`). Only this script and this README are part of the repo.
