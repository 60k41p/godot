# PLAN: GPU-writable per-instance data for RenderingServer geometry instances (`INSTANCE_EXTRA`)

## Goal

Allow the game to register a **single, GPU-writable storage buffer (SSBO)** and index it from
spatial shaders on a **per-instance** basis, for ordinary `RenderingServer` geometry instances
(spawned via `instance_create` / `instance_set_base` — **not** MultiMesh).

This gives GPU compute writes (e.g. simulation results) direct read access in the vertex shader,
with **zero CPU round-trip and zero per-instance descriptor sets**.

## Constraints / decisions

- **Scope**: Forward+ (clustered) and Forward+ Mobile renderers only. GLES3/dummy backends get
  compile-time no-ops; using `INSTANCE_EXTRA` in a shader on those backends fails shader compile,
  which is acceptable.
- **Readable in `vertex()` only** (fragment code must pass values via varyings, as usual).
- Buffer is **readonly** in the material shader (declared `restrict readonly`).
- Units are **vec4 slots** (16 bytes). The game bakes `entity * stride` into the per-instance
  offset — the engine exposes no stride API.
- The legacy per-instance uniform path (`instance uniform` block, 1 vec4 per instance, CPU-written
  into the engine-allocated buffer) is **untouched** and continues to work.
- Out-of-bounds offsets are the game's responsibility (like any SSBO); the engine only guarantees a
  valid default binding (16-byte zeroed buffer) when no user buffer is registered.

## Why not the provided patch (`references/ssbo.patch.txt`)

The patch binds `InstanceExtra` at **Set 2 Binding 1** inside **per-MultiMesh** uniform sets and
indexes it with `gl_InstanceIndex * draw_call.extra_data_stride`. For plain RS geometry instances:

- `gl_InstanceIndex` is always `0` on single-instance draws → all instances would read slot 0.
- Per-instance descriptor sets are impossible (batches share one pipeline and set).
- The patch also mentions crashiness (`"fixes, still a bit crashy"`).

Salvageable from the patch: the `INSTANCE_EXTRA` compiler desugar idea and the
`shader_language.cpp` member-type fix (allowing `[i]` on a non-array built-in).

## Design overview (Option C: "Instance userdata SSBO")

One globally-registered buffer + one per-instance offset field, mirroring the proven
`shader_uniforms_offset` / `instance_uniforms_ofs` plumbing:

```
game SSBO (entity_count * stride vec4s)  ← GPU compute writes
      ▲
      │ registered once (RID)
MaterialStorage::instance_userdata_buffer
      │ bound in base uniform set (Set 0), same set for every draw call
shader: instance_userdata.data[ ... ]
      ▲
compiler emits index = instance_userdata_index_variable + shader index i
      ▲
instance_userdata_index_variable = "instances.data[<instance index>].instance_userdata_ofs"
      ▲
per-instance field instance_userdata_ofs, set by game at spawn (vec4 units)
```

## API surface (RenderingServer)

New pure virtuals on `RenderingServer` (implemented in `RenderingServerDefault` via FUNC macros):

```cpp
void set_instance_userdata_rd_rid(RID p_buffer);                      // register the game SSBO (RenderingDevice)
RID  get_instance_userdata_rd_rid() const;                            // query (default: engine zero buffer)
void instance_geometry_set_userdata_ofs(RID p_instance, uint32_t p_ofs); // per-instance vec4 slot offset
```

- `set_instance_userdata_rd_rid` routes to `RendererMaterialStorage::set_instance_userdata_rd_rid`
  (base class; RD overrides, GLES3/dummy inherit no-op). RD version invalidates the cached base
  uniform sets (`base_uniforms_changed()`) so the new buffer is bound next frame.
- `instance_geometry_set_userdata_ofs` routes through `RendererSceneCull` →
  `RendererGeometryInstanceBase::set_instance_userdata_ofs(uint32_t)`, which stores the value and
  calls `_mark_dirty()`. Field: `uint32_t instance_userdata_ofs = 0;` (default = slot 0).

## Data flow

1. Game creates SSBO (`storage_buffer_create(entities * stride * 16)`), registers it once.
2. Compute pass writes `data[entity * stride + j]` (RenderingDevice auto-inserts barriers
   compute → draw).
3. Per spawn, game calls `instance_geometry_set_userdata_ofs(instance, entity * stride)`.
4. Frame fill: `InstanceData.instance_userdata_ofs` copied into the per-frame instance buffer
   next to `instance_uniforms_ofs`.
5. Base uniform set binds the userdata SSBO at the new Set 0 binding; shader reads
   `instance_userdata.data[int(<ofs>) + i]`.

## Shader language

New spatial vertex() built-in:

```glsl
INSTANCE_EXTRA         // TYPE_VEC4, vertex() only, not an array
INSTANCE_EXTRA[i]      // vec4 slot i of this instance's data
```

Registered in `shader_types.cpp` under spatial "vertex" built-ins.

## Compiler desugar

- Bare `INSTANCE_EXTRA` → `instance_userdata.data[int(<instance_userdata_index_variable>) + 0]`
- `INSTANCE_EXTRA[i]` → `instance_userdata.data[int(<instance_userdata_index_variable>) + int(i)]`

Where `instance_userdata_index_variable` is a new `DefaultIdentifierActions` member:
- clustered: `"instances.data[instance_index_interp].instance_userdata_ofs"`
- mobile:   `"instances.data[draw_call.instance_index].instance_userdata_ofs"`

Special-cased **before** the identifier rename path in `shader_compiler.cpp` (bare identifier
case ~line 929, `OP_INDEX` case ~line 1427). Requires the `shader_language.cpp` member-type fix
(~line 7257) so `INSTANCE_EXTRA[i]` parses as vec4-typed.

`INSTANCE_EXTRA` is only registered for vertex(); any other stage fails at parse time.

## Files to change

### 1. API / routing
| File | Change |
|---|---|
| `servers/rendering/rendering_server.h` (~767) | 3 pure virtuals |
| `servers/rendering/rendering_server.cpp` (~3242) | `bind_method` x3 + default impls |
| `servers/rendering/rendering_server_default.h` (~972) | `FUNC1`/`FUNC2`/`FUNC0` dispatch |
| `servers/rendering/rendering_method.h` (~119) | pure virtuals |
| `servers/rendering/renderer_scene_cull.h` (~1068) / `.cpp` (~1566) | routing impls |
| `servers/rendering/renderer_geometry_instance.h` (112) / `.cpp` (125) | field + setter |

### 2. InstanceData (4 copies must stay in sync)
| File | Change |
|---|---|
| `renderer_rd/forward_clustered/render_forward_clustered.h` (~340) | `uint32_t instance_userdata_ofs;` after `instance_uniforms_ofs` |
| `renderer_rd/forward_mobile/render_forward_mobile.h` (~223) | same |
| `renderer_rd/shaders/forward_clustered/scene_forward_clustered_inc.glsl` (~358) | `uint instance_userdata_ofs;` |
| `renderer_rd/shaders/forward_mobile/scene_forward_mobile_inc.glsl` (~350) | same |
| `render_forward_clustered.cpp` (~860) | fill `instance_data.instance_userdata_ofs = inst->instance_userdata_ofs` |
| `render_forward_mobile.cpp` (~2134) | same |

### 3. Buffer storage (RD `MaterialStorage`)
| File | Change |
|---|---|
| `storage/material_storage.h` (~37) | `virtual void set_instance_userdata_rd_rid(RID) {}` + `virtual RID get_instance_userdata_rd_rid() const { return RID(); }` |
| `storage_rd/material_storage.h` (~201) | fields: `RID instance_userdata_buffer; RID default_instance_userdata_buffer;` + overrides |
| `storage_rd/material_storage.cpp` (~1507) | ctor: create 16-byte zeroed default buffer; dtor: free it; setter (invalidates base sets via `RendererSceneRender::base_uniforms_changed()`); getter (registered if valid, else default) |

### 4. Base uniform set bindings
Forward+ clustered (set 0, `_update_render_base_uniform_set`, render_forward_clustered.cpp:3188):
- Renumber LTC sampler-with-texture bindings **18 → 21** (3 sites in C++, 3 sites in GLSL incl. ubershader merged block).
- Add new binding **22**: storage buffer → `MaterialStorage::get_instance_userdata_buffer()`.
- GLSL: `layout(set = 0, binding = 22, std430) restrict readonly buffer InstanceUserdata { vec4 data[]; } instance_userdata;`

Forward+ mobile (set 0, render_forward_mobile.cpp:1886):
- Renumber area_light_atlas binding **17 → 19** (1 site C++, 1 site GLSL).
- Add new binding **18**: storage buffer → userdata buffer.
- GLSL: `layout(set = 0, binding = 18, std430) restrict readonly buffer InstanceUserdata { vec4 data[]; } instance_userdata;`

(No free Set 0 binding exists in either renderer; renumbering the highest binding is the least
invasive option and avoids touching Set 1 sets which are created per-multimesh/per-particles.)

### 5. Shader compiler
| File | Change |
|---|---|
| `servers/rendering/shader_compiler.h` (~100) | `String instance_userdata_index_variable;` in `DefaultIdentifierActions` |
| `servers/rendering/shader_compiler.cpp` (~929) | bare-identifier special case |
| `servers/rendering/shader_compiler.cpp` (~1427) | `OP_INDEX` special case |
| `servers/rendering/shader_types.cpp` (~102) | `built_ins["INSTANCE_EXTRA"] = TYPE_VEC4` (vertex) |
| `servers/rendering/shader_language.cpp` (~7257) | member-type fix: non-array built-in with index → TYPE_VEC4 |
| `scene_shader_forward_clustered.cpp` (~909) | `actions.instance_userdata_index_variable = "instances.data[instance_index_interp].instance_userdata_ofs"` |
| `scene_shader_forward_mobile.cpp` (~848) | `actions.instance_userdata_index_variable = "instances.data[draw_call.instance_index].instance_userdata_ofs"` |

### 6. GLES3 / dummy
- No changes needed: dummy/GLES3 use the base `RendererMaterialStorage` no-ops; the new Set 0
  bindings only exist in the two RD renderers; shaders using `INSTANCE_EXTRA` on those backends
  fail to compile (unused feature there, matching the Forward+-only scope).

## Usage (GDExtension)

```cpp
// init
RID buf = RD::storage_buffer_create(entity_count * stride * 16);
RS::set_instance_userdata_rd_rid(buf);

// per spawn
RS::instance_geometry_set_userdata_ofs(instance, entity * stride);

// compute writes
//   data[entity * stride + j] = value (vec4)

// shader
//   vertex():
//   vec4 sim = INSTANCE_EXTRA[0];        // first vec4
//   vec3 p  = sim.xyz;  VERTEX += INSTANCE_EXTRA[1].xyz * 2.0; // etc.
```

## Verification

1. `scons platform=windows` — clean build.
2. Smoke test (Forward+): register 64-vec4 SSBO; 3 instances with ofs 0 / 8 / 16; compute writes
   distinct colors; shader displaces vertices with `INSTANCE_EXTRA[0..2]`; assert per-instance
   data reads back correctly and legacy `instance uniform` values still work.
3. Unregistered instance → reads zeros (default 16-byte buffer).
4. Confirm `INSTANCE_EXTRA` is rejected in fragment().
