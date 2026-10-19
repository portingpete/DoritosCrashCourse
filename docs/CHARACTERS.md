# Character importer

**CJ is ready to use and selected by default.** Custom characters are local packs; selecting one takes effect the next time you run `Launch.cmd`.

## Desktop workflow

1. Install Blender. Blender 5.0 was used for conversion tests. The importer finds installations beneath Program Files or lets you choose `blender.exe`.
2. Open `Character Importer.cmd` and choose a GLB, glTF, FBX or OBJ model. Keep glTF external buffers/textures or OBJ material files beside the source model.
3. Enter a display name. Leave **Auto fit** enabled for an initial attempt. Rotation is around the vertical axis; scale is a multiplier applied after the height fit.
4. Choose **Import character**. Blender runs in the background. With **Use after import** enabled, the completed pack is selected automatically. Otherwise choose it in the installed list and click **Use selected**.
5. Start `Launch.cmd`. To return to the supplied model, click **Restore CJ** or run `py -3 tools/character_importer.py select cj`.

Import jobs and their conversion logs remain beneath `build_native/characters/jobs/`. An unsuccessful conversion does not activate a partial pack. A different model cannot overwrite the bundled `cj` ID.

## Prepare a model

Use a humanoid in an upright rest pose. The importer uniformly fits its height, centers it horizontally and aligns its feet to the original reference. It transfers skin weights from the game's original reference surface, retaining the game's 72-bone skeleton and animations. It does not infer a new skeleton or automatically retarget arbitrary animation clips.

Keep one to four mesh/material groups. Each material supports a single image connected directly to the Principled BSDF Base Color, or a solid base color. Bake procedural shading and multi-image blends in Blender before conversion. Normal maps, material effects and custom animations are outside the importer’s current scope. Transparency follows the game's existing character material behavior.

Textures are normalized to noninterlaced 8-bit RGB/RGBA PNG, up to 4096×4096. Source models are limited to 64 MiB, individual pack files to 64 MiB, and complete packs to 256 MiB.

## Manual fitting

Choose **Create fitting scene**, then **Open last scene**. This saves a `.blend` containing the imported mesh and the actual local reference rig. Adjust the imported mesh to the reference in Blender. Keep the reference rig and its stored source metadata intact. Save, then choose that `.blend` as the importer’s model input.

**Surface** weights transfer from the reference geometry and are the default. **Existing** weights are useful after manually assigning vertex groups to the reference rig’s exact bone names. Fitting scenes retain their positions when reimported; the quick-fit fields are applied to new interchange-model imports.

Bone controls created by a source glTF armature are excluded from exported mesh geometry. Up to four normalized influences per vertex are retained. The Cal3D preflight also checks the original title's hardware limits: 48 bones per draw group, 64,000 hardware vertices including partition duplicates, 64,000 triangles total and 50,000 unique vertices per draw group. The report describes the actual partitioning; fitting quality still needs visual inspection in game.

## Command line

```powershell
# Convert, install and select. --id is optional; otherwise a unique ID is created.
py -3 tools/character_importer.py import-model "C:\Models\hero.glb" --name "My character" --activate

# Make a scene for manual fitting without installing a character.
py -3 tools/character_importer.py import-model "C:\Models\hero.fbx" --prepare-only

# List, validate, select, or launch a specific installed pack.
py -3 tools/character_importer.py list
py -3 tools/character_importer.py validate cj
py -3 tools/character_importer.py select cj
.\Launch.cmd --character cj

# Install a portable pack folder containing character.json.
py -3 tools/character_importer.py install-pack "C:\Models\MyCharacterPack"
```

Optional conversion arguments: `--blender PATH`, `--yaw DEGREES`, `--scale MULTIPLIER`, `--no-auto-fit`, `--weights surface|existing`, `--name NAME` and `--id lowercase-slug`.

An explicit `Launch.cmd --asset-override-dir PATH` continues to pass through to the native executable. It overrides character-pack selection for that launch.

## Pack layout and local files

A portable pack contains `character.json`, `model.cmf`, and one or more `diffuse-N.png` images. The manifest identifies schema version 1, the target skeleton hash, content hashes, display name and attribution. Pack files use canonical filenames; paths outside the pack and changed hashes are rejected. Share only assets you have permission to redistribute and preserve their attribution. Do not share fitting scenes containing the extracted original rig/reference.

Installed imports live under `build_native/characters/library/`. Selection is recorded atomically in `local/character.json`, separately from game saves. Generated configuration, material/texture declarations and character overlays stay under ignored `build_native/characters/` storage. The default mesh and textures are public replacement assets under `assets/characters/cj/`; the original skeleton, configuration and animations come from the user's verified package.

To disable a custom selection, choose Restore CJ. To remove a pack, first select CJ, close the game, then remove only that pack's directory from the local library. Save and original game files are not needed for removal.

## Troubleshooting

- **Missing native overlay:** complete the [build guide](BUILDING.md), including shader capture.
- **More than four groups:** join meshes and consolidate material slots, or bake them into fewer materials. Geometry and material groups both count.
- **Unexpected proportions or deformation:** create a fitting scene and adjust the rest pose and weights manually. Auto fitting is intended for similar humanoid proportions.
- **Unsupported material:** connect one image directly to Base Color, or use a solid Base Color. Bake complex nodes.
- **Changed pack or overlay cache:** imported data is fingerprinted. Reimport using a new ID, then select the new entry. Restore CJ if a changed selection prevents launch.
- **Blender errors:** inspect the interface log and the retained job directory. Open Blender manually to confirm it can read the model and its external textures.
