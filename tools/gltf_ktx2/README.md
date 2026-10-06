# gltf_ktx2

Converts the PNG/JPEG textures of a `.gltf` model to **BC7 KTX2** files with a full mip chain. It also updates the `.gltf` so it points at the new files.

The engine then uploads the textures without decoding them or generating mips at load time. They also use about 4× less GPU memory. For Sponza, texture memory drops from 363 MiB to 91 MiB.

## Build

The tool is a normal target in the engine's CMake project:

```sh
cmake --build --preset windows-clang-release --target gltf_ktx2
```

The executable is written to `build/<preset>/gltf_ktx2.exe`, next to `engine.exe`. Use the **release** build: Basis encoding is many times slower in debug.

## Usage

```sh
gltf_ktx2 <model.gltf | model folder> [options]
```

```sh
build/windows-clang-release/gltf_ktx2.exe models/Sponza
```

If you pass a folder, the tool uses the first `.gltf` inside it.

| Option | Default | Meaning |
|---|---|---|
| `--force` | off | Reconvert even if the `.ktx2` is newer than its source image. |
| `--jobs N` | all cores | Number of images to convert in parallel. |
| `--uastc-level 0-4` | 2 | Encoding quality vs. speed. 0 is fastest, 4 is slowest and best. |
| `--zstd 0-22` | 18 | zstd compression level for the file. 0 means no compression. It only affects file size, not GPU memory. |

The exit code is `0` on success, `1` if any texture failed, and `2` for bad arguments.

## What it changes

- **Each `.ktx2` is written next to its source image**, for example `textures/wood.png` → `textures/wood.ktx2`. Embedded images (data URIs and `bufferView` images) are written next to the `.gltf`.
- **The `.gltf` is rewritten in place.** Converted images get `"uri": "….ktx2"` and `"mimeType": "image/ktx2"`.
- **A backup is kept.** The first time the file changes, the original is saved as `<name>.gltf.bak`. To undo the conversion, copy the `.bak` back over the `.gltf`.
- **The original URI is recorded.** Each converted image gets `extras.gltf_ktx2 = { "source": "<original uri>", "colorSpace": "srgb" | "linear" }`. Later runs use this to find the PNG/JPEG again.
- **Source images are not deleted.** They are needed to reconvert.

## Running it again

The tool is safe to run any number of times:

- Textures whose `.ktx2` is newer than the PNG/JPEG are skipped.
- After you edit a source image, run the tool again and only that texture is reconverted.
- File names stay the same between runs.
- If nothing changed, the `.gltf` is left untouched.

Embedded images can only be converted once, because their PNG/JPEG data is no longer referenced afterwards. To redo them, restore the `.bak`.

## Color space

The format of each texture depends on how the materials sample it. These are the same rules the engine loader uses:

- **sRGB (`BC7_SRGB`):** `baseColorTexture`, `emissiveTexture`, `specularColorTexture`, `sheenColorTexture`, `diffuseTransmissionColorTexture`.
- **Linear (`BC7_UNORM`):** every other `*Texture`, including those inside material extensions.

If one image is used both ways, the tool writes two files (`<name>_srgb.ktx2` and `<name>_linear.ktx2`). The linear uses are pointed at a new image and texture entry.

Mips are filtered in linear light for sRGB textures. Normal maps (`normalTexture`, `clearcoatNormalTexture`) are renormalized at each mip level.

## Limitations

- **Only `.gltf` files**, not `.glb`.
- **Only PNG and JPEG sources.** Other formats and images that are already KTX2 are left alone.
- **The output only loads in this engine.** Standard glTF only allows KTX2 through `KHR_texture_basisu`, which requires Basis payloads, not BC7. Other viewers will not load the converted file.
- **Normal maps are stored as BC7.** BC5 would be a better fit but isn't supported yet.
- **The engine needs `textureCompressionBC`**, which it enables. All desktop GPUs support it.
