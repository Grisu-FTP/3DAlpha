# Task map

Choose one row, search the listed files for the relevant symbol, then read a bounded range.
Paths under `src/core/` below are shortened. Paired `.hpp`/`.cpp` modules share the named stem.
Test filters match `TEST(...)` case names: find them with `rg -n '^TEST' tests/<file>`.
Do not run every filter in this table for a single change.

| Task | Start here | Tests / deeper reference |
|---|---|---|
| Entity save/reload | `entity/persistence`, `render/world_streamer` (`bindEntities`, `snapshotEntities`), `world/chunk_cache` (`applyPlayerState`), `src/impl/storage/alpha_chunkfiles/level_dat` | `tests/entity_persistence_test.cpp`; `current-work.md` for format limits |
| Arrow, painting, boat, minecart behaviour | `entity/<name>`, `item/use`, corresponding `render/<name>_mesh`; wiring in `src/platform/ctr/main.cpp` | `tests/<name>_test.cpp`; `entity-render-a1.1.2.md` |
| Item drops / pickup / merging | `entity/item_entity`, `tick/drop`, `item/inventory`; generated `data/a1.1.2/drops.json` | `tests/item_entity_test.cpp`, `drop_test.cpp`; case names contain behaviour, not always module name |
| Falling sand/gravel | `entity/falling_block`, `tick/behaviour` (`fallingTick`) | Search `tests/tick_test.cpp` for falling cases; persistence test covers unloaded restoration |
| Signs / text | `world/sign_store`, `render/sign_mesh`, `item/use`, `src/platform/ctr/overlay.cpp` | `tests/sign_test.cpp`; sign text still not persistent |
| Block/item use or placement | `item/use`, `block/registry`, `item/registry`, `tick/behaviour` | `tests/use_test.cpp`, `placement_test.cpp`; generated `data/a1.1.2/{items,placement,selection}.json` |
| Held item / first-person hand | `render/held_item`, `Renderer::drawHeldItem` and `setHeldItem`, `editBlocks`/`hand` in `src/platform/ctr/main.cpp` | `tests/held_item_test.cpp`; `status.md` §15 for the near plane, depth map and stereo numbers |
| Top-screen messages (chat lines, entity-limit notice) | `gui/chat_log`, `render/chat_mesh`, `Renderer::drawChat`, `item::markRefusals`/`limitMessage`, `ChatSink` in `src/platform/ctr/main.cpp` | `tests/chat_log_test.cpp`; `status.md` §18; needs the pack font |
| Player skin / Skins screen | `texture/skin_list`, `texture/entity_skins` (`applyPlayerSkin`, `EntitySkin::Player`), `Menu::handleSkins`, `GameSettings::skin` | `tests/skin_list_test.cpp`, `entity_skins_test.cpp`; slim is gated on `hasSlimSkins` in `versions/<id>.json` |
| Inventory / creative palette / icons | `item/inventory`, `item/creative_palette`, `gui/item_icon`, `src/platform/ctr/overlay.cpp` | `tests/creative_test.cpp`, `item_icon_test.cpp`; item table from `tools/configure.py` |
| Movement / collision / targeting | `entity/player_body`, `entity/sweep`, `entity/ray_trace`, `block/collision`, `block/fluid_flow` | `tests/player_body_test.cpp`, `ray_trace_test.cpp`, `fluid_push_test.cpp`; `physics-a1.1.2.md` |
| Ticks / rails / redstone / fluid | `tick/tick_world`, `tick/behaviour`, `tick/rail`, `tick/redstone`, `tick/fluid` | `tests/tick_test.cpp`, `rail_test.cpp`; `tick-a1.1.2.md` |
| Chunk streaming / stale meshes / saving edits | `render/world_streamer`, `world/chunk_cache`, `render/chunk_renderer` | `tests/streamer_*`, `chunk_cache_test.cpp`; run TSan for concurrency changes |
| Folder saves / NBT / player state | `src/impl/storage/alpha_chunkfiles/{chunk_nbt,level_dat}`, `nbt/preserved`, `world/level_data` | `tests/chunk_nbt_test.cpp`, `level_dat_test.cpp`, `storage_test.cpp`; `world-format.md` |
| Packed saves / conversion | `world/any_storage`, `world/format/{packed_storage,manifest,region_file,converter}` | `tests/packed_storage_test.cpp`; `packed-worlds.md`; level data uses the same Alpha codec |
| Block geometry / lighting appearance | `mesh/{mesher,shapes,fluid,torch}`, `block/model`, `world/{lighting,light_update}` | `tests/mesher_test.cpp`, `shapes_test.cpp`; distinguish mesh lighting from world light propagation |
| Greedy meshing / cube atlas / seams | `mesh/{mesher,cube_atlas,vertex}`, `shaders/{world,quad}.v.pica`, `Atlas::initCube`/`bindCube` in `src/platform/ctr/textures.cpp` | `tests/greedy_test.cpp`, `quad_format_test.cpp`; `status.md` §22; a shader change needs the 3DS build, not only the host suite |
| GPU drawing / missing entity models | `src/platform/ctr/renderer.cpp`, `render/*_mesh`, `render/box_model`, `texture/entity_skins` | `tests/box_model_test.cpp`, `entity_skins_test.cpp`; `entity-render-a1.1.2.md`, `3ds-performance.md` |
| Texture packs / animated textures | `texture/{atlas_image,dev_art,tiled,texture_fx,compass_fx}`, `src/platform/ctr/textures.cpp` | `tests/pack_test.cpp`, `tiled_test.cpp`, `texture_fx_test.cpp`, `compass_test.cpp`; `assets.md` |
| Audio | `audio/{sound_engine,block_sound,music_ticker,vorbis_stream}`, `src/platform/ctr/audio.hpp` | `tests/step_sound_test.cpp`; `audio-a1.1.2.md`; TSan for decode-thread changes |
| Menu / HUD / input / map | `src/platform/ctr/{main,menu,overlay,hud,map_screen}`, `gui/`, `map/`, `settings/` | Search matching `tests/*`; host harness cannot verify actual 3DS controls/screens |
| World generation | `src/impl/worldgen/alpha_nobiome/`, generated version config | Search `tests/*worldgen*`, `*provider*`, `*populate*`; indexed worldgen sections in `status.md` |
| Build / version data / reference derivation | `CMakeLists.txt`, `Makefile`, `versions/`, `tools/{configure.py,extract_blocks.py,javap.py,genref.java}` | `build-versions.md`, `working-guide.md`; jar derivation is a maintainer action, never a runtime prerequisite |

## Bounded discovery

```sh
rg -n 'snapshotEntities|saveNow' src/core/render/world_streamer.cpp
rg -n '^TEST.*(save|entity)' tests/entity_persistence_test.cpp
rg -n -i 'minecart|persistence' docs/code-map.md docs/doc-index.md
# Use the returned section range, for example:
sed -n '1,75p' src/core/entity/persistence.hpp
```

`src/platform/ctr/main.cpp` owns game-loop wiring. Core logic may be correct but never called;
trace placement → pool → tick → rendering or save, as appropriate. Test file names and generated
indexes are discovery aids, not proof of coverage. Avoid full-tree `find`, build-directory scans,
generated vector dumps, and whole-file reads of the large status/physics/rendering documents.
