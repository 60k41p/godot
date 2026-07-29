# GPU-Driven Instance Rendering via Compute→MultiMesh SSBO

## Architecture Overview

```
[ECS Compute Sim] → [GPU Culling/LOD/Compaction] ──writes──→ multimesh->buffer          (Set=2, Bind=0, transforms SSBO)
                                                        ──writes──→ multimesh->extra_data    (Set=2, Bind=1, per-instance uniforms SSBO)
                                                        ──writes──→ multimesh->command_buffer (indirect draw buffer)
                                                                         ↓
                                                        Godot Forward Clustered Renderer
                                                        (lighting, shadows, materials, GI, fog, post, all built-in)
```

**Key insight:** `multimesh_get_buffer_rd_rid()` and `multimesh_get_command_buffer_rd_rid()` are already exposed through `ClassDB` (`rendering_server.cpp:2538-2539`). The MultiMesh buffers are standard RD storage buffers with `BUFFER_USAGE_STORAGE_BIT` — writable from compute shaders. The engine's dirty-region updater only touches the buffer if CPU-side writes occur; if we never call CPU write APIs, the engine leaves our GPU-populated buffers alone.

---

## Step 1: Spatial Partitioning (Grid Cells)

World divided into grid cells. Each cell produces tight MultiMesh AABBs for Godot's BVH.

### Grid Constants

| Constant | Value | Rationale |
|---|---|---|
| `CELL_SIZE` | 64 m | Balances BVH granularity vs GPU culling batch size |
| `CELL_MAX_ENTITIES` | 65536 | Max per MultiMesh (GPU memory: 65536 × 48 bytes ≈ 3 MB per cell per LOD) |
| `UNIQUE_MESHES` | 2 | Per your spec |
| `LOD_LEVELS` | 4 | Per your spec |
| `MAX_CELLS` | 256 | Conservative; 200K entities / 8K per cell ≈ 25 cells |

### Grid Cell Structure (GDExtension C++)

```cpp
struct GridCell {
    // Godot scene handles
    RID scenario;                       // scenario this cell belongs to
    RID multimesh[UNIQUE_MESHES][LOD_LEVELS];  // one MultiMesh per mesh × LOD
    RID mm_instance[UNIQUE_MESHES][LOD_LEVELS]; // MultiMeshInstance3D RID

    // Cached RD buffer RIDs (queried once after creation)
    RID transform_buffer[UNIQUE_MESHES][LOD_LEVELS];
    RID extra_data_buffer[UNIQUE_MESHES][LOD_LEVELS];
    RID command_buffer[UNIQUE_MESHES][LOD_LEVELS];

    // Cached uniform sets for compute shader
    RID transform_extra_set[UNIQUE_MESHES][LOD_LEVELS];  // Set 1 in compute: B0=transforms, B1=extra
    RID cmd_buffer_set[UNIQUE_MESHES][LOD_LEVELS];       // Set 2 in compute

    // Per-cell compute counter buffer (for instance compaction)
    RID counter_buffer;

    // CPU-side entity index ranges for this cell
    uint32_t entity_start;
    uint32_t entity_count;

    AABB cell_aabb;
    bool active;

    // LOD distance thresholds (per-mesh)
    float lod_thresholds[LOD_LEVELS - 1];
};
```

### Grid Management Flow

```cpp
// On world creation:
for each cell in grid:
    cell.scenario = RS::get_singleton()->scenario_create();

    for each mesh_id, lod:
        cell.multimesh[mesh_id][lod] = RS::get_singleton()->multimesh_create();
        RS::get_singleton()->multimesh_allocate_data(
            cell.multimesh[mesh_id][lod],
            CELL_MAX_ENTITIES,
            RS::MULTIMESH_TRANSFORM_3D,  // 3D transforms
            false,                        // use_colors (unnecessary — extra data in Set 2 B1 SSBO)
            false,                        // use_custom_data (unnecessary)
            true);                        // use_indirect ← CRITICAL

        RS::get_singleton()->multimesh_set_mesh(
            cell.multimesh[mesh_id][lod],
            lod_meshes[mesh_id][lod]);    // LOD variant mesh

        RS::get_singleton()->multimesh_set_custom_aabb(
            cell.multimesh[mesh_id][lod],
            cell.cell_aabb);              // tight AABB for BVH

        // Cache RIDs for compute shader writing — call once
        cell.transform_buffer[mesh_id][lod] =
            RS::get_singleton()->multimesh_get_buffer_rd_rid(
                cell.multimesh[mesh_id][lod]);
        cell.command_buffer[mesh_id][lod] =
            RS::get_singleton()->multimesh_get_command_buffer_rd_rid(
                cell.multimesh[mesh_id][lod]);

        // Per-MultiMesh extra data buffer (Set 2 B1), same size as transforms
        cell.extra_data_buffer[mesh_id][lod] =
            RD::get_singleton()->storage_buffer_create(
                CELL_MAX_ENTITIES * UNIFORM_STRIDE * sizeof(float) * 4);

        // Register extra data buffer with the MultiMesh for forward shader binding
        RS::get_singleton()->multimesh_set_extra_data_rd_rid(
            cell.multimesh[mesh_id][lod],
            cell.extra_data_buffer[mesh_id][lod]);

        // Create scene instance
        cell.mm_instance[mesh_id][lod] =
            RS::get_singleton()->instance_create2(
                cell.multimesh[mesh_id][lod],
                cell.scenario);

        // Create compute uniform set binding both transforms and extra data at Set 1
        {
            Vector<RD::Uniform> uniforms;
            RD::Uniform u;
            u.binding = 0;
            u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
            u.append_id(cell.transform_buffer[mesh_id][lod]);
            uniforms.push_back(u);
            RD::Uniform u2;
            u2.binding = 1;
            u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
            u2.append_id(cell.extra_data_buffer[mesh_id][lod]);
            uniforms.push_back(u2);
            cell.transform_extra_set[mesh_id][lod] =
                RD::get_singleton()->uniform_set_create(
                    uniforms, culling_shader, 1);
        }

    cell.counter_buffer = RD::get_singleton()->storage_buffer_create(
        sizeof(uint32_t) * UNIQUE_MESHES * LOD_LEVELS);
    
    cell.active = true;
```

**IMPORTANT:** After `allocate_data`, never call:
- `multimesh_instance_set_transform*()` — creates CPU data_cache + dirty regions
- `multimesh_instance_set_color()` — same
- `multimesh_set_buffer()` — same
- `multimesh_set_visible_instances()` — overwrites the indirect command buffer

The engine only uploads data if `data_cache.size() > 0 && dirty_regions != nullptr`. Since we never populate `data_cache`, `_update_dirty_multimeshes()` (`mesh_storage.cpp:2257`) skips our buffers entirely.

---

## Step 2: Exact MultiMesh Buffer Layout

Your compute shader must write data in exactly the format the vertex shader expects.

### Transforms Buffer (Set=2, Binding=0)

From `mesh_storage.cpp:1573-1576`:

```
Stride = 12 (for 3D, no color, no custom_data) = 3 vec4s
```

#### Per-Instance Layout (transform only, stride = 3 vec4s)

```glsl
// Instance N offset = N * 3
// From scene_forward_clustered_inc.glsl:490-493
layout(set = 2, binding = 0, std430) restrict readonly buffer Transforms {
    vec4 data[];
} transforms;
```

| Slot | GLSL Access | Contents | C++ Equivalent |
|---|---|---|---|
| `offset + 0` | `transforms.data[offset]` | `row0.xyz = basis[0], row0.w = origin.x` | `t.basis.rows[0][0..2], t.origin.x` |
| `offset + 1` | `transforms.data[offset + 1]` | `row1.xyz = basis[1], row1.w = origin.y` | `t.basis.rows[1][0..2], t.origin.y` |
| `offset + 2` | `transforms.data[offset + 2]` | `row2.xyz = basis[2], row2.w = origin.z` | `t.basis.rows[2][0..2], t.origin.z` |

The vertex shader reads these at `scene_forward_clustered.glsl:306-327`:

```glsl
uint stride = multimesh_stride();           // = 3 (no color, no custom_data)
uint offset = stride * (gl_InstanceIndex + multimesh_offset);

// Row-major: 3 vec4s form the 3x4 transform matrix
matrix = mat4(transforms.data[offset + 0],
              transforms.data[offset + 1],
              transforms.data[offset + 2],
              vec4(0.0, 0.0, 0.0, 1.0));

// No color or custom_data reads (specialization constants disable them)
```

Per-instance extra data (colors, uniforms, etc.) is accessed via the dedicated Set 2 B1 SSBO — see Step 4.

### Indirect Command Buffer

From `mesh_storage.h:62` and `mesh_storage.cpp:1678-1687`:

```
INDIRECT_MULTIMESH_COMMAND_STRIDE = 5 uint32s per mesh surface
```

```glsl
// Command buffer for a mesh with S surfaces:
// uint cmd[S * 5];

// Surface i:
cmd[i * 5 + 0] = vertex_count   (or index count if indexed)
cmd[i * 5 + 1] = instance_count (update to visible count each frame)
cmd[i * 5 + 2] = first_vertex
cmd[i * 5 + 3] = first_instance
cmd[i * 5 + 4] = unused
```

The draw call at `render_forward_clustered.cpp:609`:

```cpp
RD::get_singleton()->draw_list_draw_indirect(
    draw_list,
    index_array_rd.is_valid(),
    command_buffer,
    surface_index * sizeof(uint32_t) * 5,  // offset
    1,                                       // draw count
    0);                                      // stride (tightly packed)
```

**For command buffer population in compute:** After compaction, write the visible count to `cmd[surface * 5 + 1]` for each surface. The vertex count (field 0) is set once during `multimesh_set_mesh` and never needs updating.

---

## Step 3: ECS Data on GPU

### Entity Data Buffer

Created and managed by your GDExtension code. Layout is your choice; example:

```glsl
// Entity state buffer (created by GDExtension, read by compute culling)
layout(set = 0, binding = 0, std430) readonly buffer EntityState {
    vec4 pos_vel[];           // .xyz = world position, .w = velocity magnitude
    vec4 rot_scale[];         // rotation quaternion or Euler angles, .w = uniform scale
    vec4 color_extra[];       // per-entity color tint (written to Set 2 B1 extra data)
    vec4 uniforms[];          // per-entity uniform data (written to Set 2 B1 extra data)
    uint mesh_id[];           // which of the 2 meshes this entity uses
    uint flags[];             // visibility, animation state, etc.
} entity_state;

// Output: per-entity LOD + compaction data (written by compute culling)
layout(set = 0, binding = 1, std430) buffer CullResult {
    uint visible_count;          // atomic counter
    uint pad0, pad1, pad2;
    // Packed output: indices into entity_state for visible entities per LOD
    uint visible_indices[];      // total size = CELL_MAX_ENTITIES
} cull_result;
```

### Creation from GDExtension

```cpp
size_t entity_data_size = CELL_MAX_ENTITIES * (sizeof(float) * 8 + sizeof(uint32_t) * 2);
RID entity_buffer = RD::get_singleton()->storage_buffer_create(entity_data_size);
```

### CPU Update (per frame, before compute dispatch)

Your ECS updates entity transforms/state on the GPU:

```cpp
// For dynamic persistent buffers (no staging copy):
RID dyn_buffer = RD::get_singleton()->storage_buffer_create(
    size, {},
    RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT,
    RD::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT);

// Map and copy each frame:
void *mapped = RD::get_singleton()->buffer_map(dyn_buffer);
memcpy(mapped, ecs_entity_data, size);
// No unmap needed for persistent buffers

// For regular buffers (uses staging):
RD::get_singleton()->buffer_update(entity_buffer, 0, size, ecs_entity_data);
```

**Performance tip:** Use `BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT` for the entity state buffer. This creates a CPU-mappable, persistently mapped buffer with no driver-side staging copy. The write is a direct `memcpy` into GPU-visible memory.

---

## Step 4: Per-Instance Shader Uniforms

We need per-draw-instance uniform data (<16 vec4 per entity) accessible from Godot's forward vertex/fragment shaders. The key finding from our research: **Set 2, Binding 1 of the forward clustered shader is completely unused.** Set 2 is the per-MultiMesh uniform set — each MultiMesh already has its own transforms buffer at Binding 0 and its own uniform set created by `multimesh_get_3d_uniform_set`. Adding Binding 1 for extra data means each MultiMesh gets its own extra data buffer indexed by `gl_InstanceIndex` — no collision across cells/meshes/LODs.

### Recommended: Set 2 Binding 1 Extra Data SSBO (~20 LOC engine patch)

**Engine patch:**
1. Add to `scene_forward_clustered_inc.glsl:494`:
```glsl
layout(set = 2, binding = 1, std430) restrict buffer InstanceExtra {
    vec4 data[];
} instance_extra;
```

2. Add `extra_data_buffer` field to `MultiMesh` struct in `mesh_storage.h`:
```cpp
RID extra_data_buffer;
```

3. Add a public setter on `RenderingServer`:
```cpp
void multimesh_set_extra_data_rd_rid(RID p_multimesh, RID p_buffer);
```

4. In `mesh_storage.cpp`, implement the setter and modify `multimesh_get_3d_uniform_set()` to include the extra buffer at Binding 1 if valid:
```cpp
// In multimesh_get_3d_uniform_set(), after binding 0:
if (multimesh->extra_data_buffer.is_valid()) {
    RD::Uniform u2;
    u2.binding = 1;
    u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
    u2.append_id(multimesh->extra_data_buffer);
    uniforms.push_back(u2);
}
// Invalidate uniform_set_3d so it rebuilds on next access
```

**Usage in material shader:**
```glsl
// vertex() or fragment():
// gl_InstanceIndex indexes both transforms.data[] and instance_extra.data[]
vec4 my_uniform = instance_extra.data[gl_InstanceIndex * UNIFORM_STRIDE + slot_index];
```

Both buffers are indexed by `gl_InstanceIndex` — no offset, no entity_id, no indirection.

### Why This Beats The Alternatives

| Approach | Data Transfer | Access Overhead | Binding Cost | Capacity |
|---|---|---|---|---|
| **Set 2 B1 SSBO + stride 3** (Plan B) | 2 GPU writes (transforms + extra data) | Direct indexed read `[slot * S + i]` — **no indirection** | Set 2 already bound per-surface; adding B1 is free | Unlimited per MultiMesh |
| custom_data → texture lookup | 2 GPU writes + 1 extra slot per instance | Dependent read + coordinate math + texel unit | Material uniform set (per-surface) | Unlimited |
| Godot's 16×vec4 `instance uniform` | 0 writes (CPU only) | Direct read, but **shared across instances** | Set 0 B12 (always bound) | 16 vec4 per MM node — **not per entity** |
| userdata_count patch (~250 LOC) | 1 GPU write (merged buffer) | Direct read (same stride) | Set 2 B0 (always bound) | N×vec4 per instance |

With Plan B, `gl_InstanceIndex` indexes both buffers directly — no entity_id, no `floatBitsToUint`, no dependent indirection. Extra wins:
- **Compact MultiMesh** — stride=3 saves 40% MM buffer memory vs stride=5
- **No collision** — each MultiMesh has its own extra buffer, no global indexing issue
- **No texel addressing math** — `data[slot * S + i]` vs `texelFetch(ivec2(idx % W, idx / W), 0)`
- **SSBO cache lines** — contiguous vec4 loads hit L2 better than texture unit for pure data
- **Compute-writable** — same buffer written by your culling shader, no CPU involvement
- **No hard cap** — vs 16-float limit on `instance_uniforms_ofs`

### Fallback: 0 Engine Changes

If you must avoid patches, use MultiMesh stride=5 (color + custom_data enabled). Pack entity data into `custom_data` (1 vec4) and/or store an index for looking up more data from a texture or SSBO bound through the material's shader parameters. The Godot shader language doesn't support raw `buffer` declarations, so a `texture2D` + `texelFetch` is the simplest approach. You can also pre-pack into the 16 `instance uniform` slots, but those are shared per MultiMesh node — not per entity.

---

## Step 5: Compute Shader — Culling + LOD + Compaction

### GLSL Structure

```glsl
#version 450
#[compute]
#define LOCAL_SIZE 256

// Push constants (≤ 128 bytes)
layout(push_constant, std430) uniform PC {
    uint cell_entity_start;    // first entity index in this cell
    uint cell_entity_count;    // number of entities in this cell
    uint mesh_id;              // which mesh (0 or 1) to process
    uint lod_count;            // number of LOD levels
    float lod_ranges[3];       // LOD0→LOD1, LOD1→LOD2, LOD2→LOD3 transition distances
    float cell_center_x;       // for AABB computation
    float cell_center_y;
    float cell_center_z;
    float cell_half_size;
    uint mm_stride;            // = 3 (no color, no custom_data)
    uint surface_count;        // surfaces in this mesh
    uint unif_stride;          // vec4s per entity in extra data buffer
} pc;

// Set 0, Binding 0: Read-only entity state (from your ECS)
layout(set = 0, binding = 0, std430) readonly buffer EntityState {
    vec4 pos_vel[];
    vec4 rot_scale[];
    uint mesh_id_buf[];
    uint flags[];
} state;

// Set 0, Binding 1: Camera data (updated by CPU each frame via buffer_update)
struct CameraData {
    vec4 frustum_planes[6];
    vec3 camera_pos;
    float near;
    vec3 camera_dir;
    float far;
    vec2 screen_size;
    float mesh_radius[2];   // bounding sphere radius for each mesh
};
layout(set = 0, binding = 1, std430) readonly buffer Camera {
    CameraData cam;
} camera;

// Set 1, Binding 0: MultiMesh transform output (writable — multimesh->buffer)
layout(set = 1, binding = 0, std430) buffer MMOut {
    vec4 data[];
} mm_out;

// Set 2, Binding 0: Indirect command buffer (writable — multimesh->command_buffer)
layout(set = 2, binding = 0, std430) buffer IndirectCmd {
    uint cmd[];
} indirect;

// Set 3, Binding 0: Per-LOD atomic counters
layout(set = 3, binding = 0, std430) buffer Counters {
    uint lod_counters[];
} counters;

// Set 4, Binding 0: Per-instance extra data output (writable — per-MultiMesh Set 2 B1 buffer)
layout(set = 4, binding = 0, std430) buffer ExtraOut {
    vec4 data[];
} extra_out;

// ─── Main ───

shared uint local_visible;
shared uint local_offsets[LOCAL_SIZE];

void main() {
    uint gtid = gl_GlobalInvocationID.x;
    uint tid = gl_LocalInvocationIndex;

    if (tid == 0) {
        local_visible = 0;
    }
    barrier();
    memoryBarrierShared();

    bool visible = false;
    uint lod = 0;

    if (gtid < pc.cell_entity_count) {
        uint global_idx = pc.cell_entity_start + gtid;

        // 1. Read entity state
        vec3 pos = state.pos_vel[global_idx].xyz;
        float scale = state.rot_scale[global_idx].w;

        // 2. Basic frustum culling against 6 planes
        float mesh_r = camera.cam.mesh_radius[pc.mesh_id] * scale;
        visible = true;
        for (int i = 0; i < 6; i++) {
            vec4 p = camera.cam.frustum_planes[i];
            float d = dot(vec4(pos, 1.0), p);
            if (d < -mesh_r) { visible = false; break; }
        }

        // 3. LOD selection by distance
        if (visible) {
            float dist = distance(pos, camera.cam.camera_pos);
            lod = 0;
            for (uint l = 0; l < pc.lod_count - 1; l++) {
                if (dist > pc.lod_ranges[l]) lod = l + 1;
            }
        }
    }

    // 4. Workgroup-level prefix sum for compaction
    //    Each visible thread stores 1, then we compute prefix sums
    //    to determine slot assignments within this workgroup.
    if (visible) {
        local_offsets[tid] = 1;
    } else {
        local_offsets[tid] = 0;
    }

    // Prefix sum (within workgroup)
    for (uint offset = 1; offset < LOCAL_SIZE; offset <<= 1) {
        memoryBarrierShared();
        barrier();
        uint v;
        if (tid >= offset) {
            v = local_offsets[tid - offset];
        }
        memoryBarrierShared();
        barrier();
        if (tid >= offset) {
            local_offsets[tid] += v;
        }
    }
    barrier();
    memoryBarrierShared();

    // 5. Compact: each visible thread gets a unique slot within the workgroup
    uint local_slot = local_offsets[tid] - 1;
    uint total_in_wg = local_offsets[LOCAL_SIZE - 1];

    // 6. Global slot via atomic counter (per-LOD)
    uint lod_counter_offset = pc.mesh_id * pc.lod_count + lod;
    uint global_slot = 0;

    if (visible) {
        // atomicAdd returns the previous value, which is our slot
        if (tid == 0 && total_in_wg > 0) {
            global_slot = atomicAdd(counters.lod_counters[lod_counter_offset], total_in_wg);
            // Only thread 0 does the global atomic — then share via shared
        }
        // For simplicity: each thread does its own atomicAdd
        // (more expensive but simpler — optimize with shared atomics later)
        global_slot = atomicAdd(counters.lod_counters[lod_counter_offset], 1);
    }

    // 7. Write transform to MultiMesh output buffer and extra data to Set 2 B1 buffer
    if (visible) {
        uint stride = pc.mm_stride;  // = 3 (no color, no custom_data)
        uint out_offset = global_slot * stride;

        // Build 3x4 row-major transform
        float cos_r = 1.0, sin_r = 0.0;  // (replace with actual rotation)
        vec3 basis0 = vec3(cos_r, 0.0, sin_r) * scale;
        vec3 basis1 = vec3(0.0, 1.0, 0.0) * scale;
        vec3 basis2 = vec3(-sin_r, 0.0, cos_r) * scale;

        mm_out.data[out_offset + 0] = vec4(basis0, pos.x);
        mm_out.data[out_offset + 1] = vec4(basis1, pos.y);
        mm_out.data[out_offset + 2] = vec4(basis2, pos.z);

        // Write extra per-instance data at same compacted slot
        uint unif_ofs = global_slot * pc.unif_stride;
        extra_out.data[unif_ofs + 0] = state.color_extra[global_idx];  // color tint etc.
        extra_out.data[unif_ofs + 1] = state.uniforms[global_idx];     // your per-entity uniforms
        // ... more slots up to unif_stride
    }
}
```

### Second Compute Pass: Update Indirect Command Buffer

```glsl
#version 450
#[compute]
#define LOCAL_SIZE 64

layout(push_constant, std430) uniform PC {
    uint mesh_id;
    uint lod_count;
    uint max_count;          // cell_max_entities
    uint surface_count;      // mesh surface count
    uint mm_stride;
} pc;

layout(set = 2, binding = 0, std430) buffer IndirectCmd { uint cmd[]; } indirect;
layout(set = 3, binding = 0, readonly) buffer Counters { uint lod_counters[]; } counters;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.lod_count) return;

    uint counter_idx = pc.mesh_id * pc.lod_count + i;
    uint count = counters.lod_counters[counter_idx];

    // Clamp to max
    count = min(count, pc.max_count);

    // Write instanceCount (field 1) for each surface
    for (uint s = 0; s < pc.surface_count; s++) {
        uint cmd_offset = s * 5;  // only 1 draw per LOD, but surfaces are indexed
        // Actually: each LOD has its own MultiMesh with its own command buffer.
        // The command buffer has 5 uint32s per surface.
        indirect.cmd[s * 5 + 1] = count;
    }
}
```

### Dispatch Sequence (GDExtension, per Cell per Mesh)

```cpp
void dispatch_cell_culling(GridCell &cell, uint32_t mesh_id, uint32_t frame_index) {
    for (uint lod = 0; lod < LOD_LEVELS; lod++) {
        // Reset atomic counter for this mesh+LOD
        uint zero = 0;
        uint counter_idx = mesh_id * LOD_LEVELS + lod;
        RD::get_singleton()->buffer_update(
            cell.counter_buffer,
            counter_idx * sizeof(uint32_t),
            sizeof(uint32_t),
            &zero);
    }

    // Dispatch culling compute (writes transforms + fills counters)
    {
        RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();

        RD::get_singleton()->compute_list_bind_compute_pipeline(cl, culling_pipeline);

        // Set 0: entity state + camera data
        RD::get_singleton()->compute_list_bind_uniform_set(cl, entity_data_set, 0);
        // Set 1: transforms output + extra data output (per-mesh-per-lod)
        RD::get_singleton()->compute_list_bind_uniform_set(cl,
            cell.transform_extra_set[mesh_id][lod], 1);
        // Set 2: indirect command buffer (multimesh->command_buffer)
        RD::get_singleton()->compute_list_bind_uniform_set(cl,
            cell.cmd_buffer_set[mesh_id][lod], 2);
        // Set 3: counters
        RD::get_singleton()->compute_list_bind_uniform_set(cl, cell.counter_set, 3);

        CullingPushConstants pc;
        pc.cell_entity_start = cell.entity_start;
        pc.cell_entity_count = cell.entity_count;
        pc.mesh_id = mesh_id;
        pc.mm_stride = 3;       // no color, no custom_data
        pc.unif_stride = UNIFORM_STRIDE;
        pc.surface_count = mesh_surface_count[mesh_id];
        // ... fill other fields ...
        RD::get_singleton()->compute_list_set_push_constant(cl, &pc, sizeof(pc));

        uint group_count = (cell.entity_count + 255) / 256;
        RD::get_singleton()->compute_list_dispatch(cl, group_count, 1, 1);

        RD::get_singleton()->compute_list_end();
    }

    // Dispatch indirect buffer update compute (reads counter, writes command buffer)
    {
        RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();

        RD::get_singleton()->compute_list_bind_compute_pipeline(cl, indirect_update_pipeline);

        RD::get_singleton()->compute_list_bind_uniform_set(cl, cell.cmd_update_set, 2);
        RD::get_singleton()->compute_list_bind_uniform_set(cl, cell.counter_set, 3);

        IndirectPushConstants pc;
        pc.mesh_id = mesh_id;
        pc.lod_count = LOD_LEVELS;
        pc.max_count = CELL_MAX_ENTITIES;
        pc.surface_count = mesh_surface_count[mesh_id];
        RD::get_singleton()->compute_list_set_push_constant(cl, &pc, sizeof(pc));

        RD::get_singleton()->compute_list_dispatch(cl, 1, 1, 1); // single workgroup

        RD::get_singleton()->compute_list_end();
    }
}
```

**Important:** Each LOD level gets its own MultiMesh with its own command buffer. The culling shader dispatches per-LOD-level, or routes to different output buffers.

---

## Step 6: Compute Shader Setup from GDExtension

### Compiling the GLSL

```cpp
RenderingDevice *rd = RD::get_singleton();

// 1. Compile GLSL to SPIR-V
String error;
Vector<uint8_t> culling_spirv = rd->shader_compile_spirv_from_source(
    RD::SHADER_STAGE_COMPUTE,
    culling_glsl_source,
    RD::SHADER_LANGUAGE_GLSL,
    &error);
ERR_FAIL_COND_MSG(culling_spirv.is_empty(), "Culling shader compile failed: " + error);

// 2. Create shader from SPIR-V
Vector<RD::ShaderStageSPIRVData> stages;
stages.push_back({RD::SHADER_STAGE_COMPUTE, culling_spirv});
RID culling_shader = rd->shader_create_from_spirv(stages, "entity_culling");

// 3. Create compute pipeline
RID culling_pipeline = rd->compute_pipeline_create(culling_shader);

// 4. Create uniform sets
//    Set 0: entity state buffer + camera data buffer
Vector<RD::Uniform> set0_uniforms;
{
    RD::Uniform u;
    u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
    u.binding = 0;
    u.append_id(entity_state_buffer);
    set0_uniforms.push_back(u);
}
{
    RD::Uniform u;
    u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
    u.binding = 1;
    u.append_id(camera_data_buffer);
    set0_uniforms.push_back(u);
}
RID set0_handle = rd->uniform_set_create(set0_uniforms, culling_shader, 0);

// Set 1: transforms output (per-cell, per-LOD)
// Set 2: indirect buffer (per-cell, per-LOD)
// Set 3: counters (per-cell)

// Store for reuse each frame (don't recreate every frame)
```

### Camera Data Buffer (Updated Each Frame)

```cpp
struct CameraDataGPU {
    float frustum_planes[6][4];  // 6 vec4s
    float camera_pos[4];         // .w = padding
    float near;
    float far;
    float screen_size[2];
    float mesh_radius[2];        // bounding sphere radius per mesh
};

CameraDataGPU cam_data;
// ... fill from camera ...

RD::get_singleton()->buffer_update(
    camera_data_buffer, 0, sizeof(cam_data), &cam_data);
```

---

## Step 7: Timing — CompositorEffect

Use a CompositorEffect with `PRE_OPAQUE` callback to run compute before Godot's opaque draw pass.

### Setup

```cpp
RID effect = RS::get_singleton()->compositor_effect_create();
RS::get_singleton()->compositor_effect_set_callback(
    effect,
    RS::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE,
    Callable(entity_renderer, "_on_pre_opaque"));
RS::get_singleton()->compositor_effect_set_enabled(effect, true);
```

### Callback Implementation

```cpp
void EntityRenderer::_on_pre_opaque() {
    // 1. Update camera data (CPU→GPU)
    update_camera_data();

    // 2. Reset atomic counters for all cells
    reset_counters();

    // 3. Dispatch culling compute for each active cell
    for (auto &cell : active_cells) {
        dispatch_cell_culling(cell, 0 /* mesh_id */);
        dispatch_cell_culling(cell, 1 /* mesh_id */);
    }

    // 4. Dispatch indirect buffer update for each cell
    // (Could also be rolled into step 3 as a second dispatch)
    for (auto &cell : active_cells) {
        dispatch_indirect_update(cell);
    }

    // At this point:
    // - multimesh->buffer has compacted visible transforms
    // - multimesh->command_buffer has correct instance counts
    // - Godot's renderer will draw these when it reaches the MultiMesh instances

    // No CPU involvement needed during the actual draw call.
}
```

### Render Graph Synchronization

The RD render graph automatically handles barriers between compute writes and draw reads. The sequence is:

1. `compute_list_begin()` → dispatch → `compute_list_end()` (in `PRE_OPAQUE`)
2. Forward clustered renderer begins its draw list: `draw_list_begin()` → draw → `draw_list_end()`

The render graph sees: resource was written by compute → resource is read by draw → inserts `VkPipelineStageFlags` barrier automatically. No explicit barrier needed.

**Performance consideration:** If the compute dispatches take too long, they stall the start of the draw list. To hide latency, consider pipelining the simulation compute one frame ahead:

```
Frame N:
  - Kick simulation compute for frame N+1 (runs async)
  - Use results from frame N-1 simulation for frame N culling+rendering

Frame N+1:
  - Kick simulation compute for frame N+2
  - Use results from frame N simulation for frame N+1 culling+rendering
```

This requires double- (or triple-) buffering the entity state buffer.

---

## Step 8: What Works and What Doesn't

### Fully Supported (No Engine Changes)

| Feature | Status | Why |
|---|---|---|
| Lighting (omni, spot, directional) | ✅ | Godot's Set 0 scene uniforms |
| Shadow mapping (all passes) | ✅ | Automatically handled by forward clustered renderer |
| Depth pre-pass | ✅ | Part of standard render pipeline |
| SDFGI / VoxelGI / Lightmap GI | ✅ | Automatically, per scene instance |
| Fog volumes | ✅ | Same pipeline |
| Decals | ✅ | Same pipeline |
| Reflection probes | ✅ | Same pipeline |
| Motion vectors | ✅ (with caveat) | Need to manage `motion_vectors_current_offset` in push constant |
| Materials and shaders | ✅ | Full `shader_type spatial` support |
| Post-processing (AO, SSR, TAA, etc.) | ✅ | Not affected by instancing path |
| Transparent objects | ✅ | Handled by separate alpha render list |
| Frustum culling (grid-level) | ✅ | Godot's BVH against MultiMesh custom AABB |
| Occlusion culling | ✅ | BVH-level, same as any geometry instance |
| Per-instance LOD | ✅ | Via MultiMesh splitting (one per LOD per cell) |
| Per-instance frustum culling | ✅ | Your compute shader |
| CPU overhead per entity | ✅ Zero | All culling and transform management on GPU |

### Requires Extra Attention

| Feature | Caveat | Workaround |
|---|---|---|
| Motion vectors | `multimesh->motion_vectors_current_offset` defaults to 0 | Set `multimesh_set_buffer_interpolated()` once to enable motion vectors, or patch `multimesh_enable_motion_vectors` (minor). Without enablement, motion vectors produce no output (acceptable if TAA is off) |
| Per-MultiMesh instance uniforms | Shared across all draw-instances within one MultiMesh | Use Set 2 B1 extra data SSBO (Plan B, ~20 LOC patch) for full per-entity uniforms with direct access at `gl_InstanceIndex`; fallback: custom_data as lookup index (0 engine changes) |
| MultiMesh AABB recomputation | Engine normally recomputes on CPU writes | Set `custom_aabb` manually; disable motion vectors if not needed |
| Physics interpolation | MultiMesh physics interpolation expects CPU data | Not applicable (your sim is GPU-based) |

---

## Step 9: Potential Issues and Mitigations

### Issue 1: Engine Overwrites the Buffer

**Scenario:** Some engine code path calls `_update_dirty_multimeshes()` and overwrites our GPU-populated buffer.

**Analysis:** `_update_dirty_multimeshes()` only runs for MultiMeshes where `data_cache.size() > 0` and `dirty_regions` is set (`mesh_storage.cpp:2257`). The `data_cache` is only populated by CPU-side write APIs (`instance_set_transform`, `set_buffer`, etc.). Since we never call those, `data_cache` stays empty and `_update_dirty_multimeshes` skips our MultiMesh.

**Mitigation:** Verify with a one-frame `buffer_get_data()` check during development:
```cpp
Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(multimesh->buffer);
// Compare against what your compute shader wrote
```

### Issue 2: Indirect Command Buffer Corruption

**Scenario:** `multimesh_set_visible_instances()` is called (by engine or by accident), which calls `buffer_update()` on the command buffer, overwriting our GPU-computed instance count.

**Mitigation:** Never call `multimesh_set_visible_instances()`. The default `visible_instances = -1` means "use all instances up to `instances`" — which is fine since our indirect command buffer controls the actual count via the `instanceCount` field.

### Issue 3: Motion Vector Initialization

**Scenario:** If motion vectors are enabled in the project, the engine expects the buffer to have space for a second copy of transforms (previous frame). The forward clustered renderer reads `multimesh_motion_vectors_current_offset` from push constants (`render_forward_clustered.cpp:575-582`).

**Analysis:** If `motion_vectors_enabled` is false on the MultiMesh, the offsets default to 0 and the renderer reads from offset 0 (your current frame data for both current and previous). This produces no motion vectors (acceptable if TAA is off).

**Mitigation:** If you need motion vectors, call `multimesh_set_buffer_interpolated()` once with your initial data. This enables motion vectors and sets up the buffer layout. After that, your compute shader must write to both halves of the buffer (offset 0 for current, offset `stride * instances` for previous).

### Issue 4: Push Constant Overflow

**Scenario:** The culling shader push constants exceed 128 bytes.

**Mitigation:** `MAX_PUSH_CONSTANT_SIZE = 128` (`rendering_device.h:1115`). For the culling shader, we need: entity range (8 bytes), mesh/lod config (12 bytes), lod ranges (12 bytes), cell center (16 bytes), misc (8 bytes) ≈ 56 bytes — well within limits.

### Issue 5: Atomic Counter Reset Overhead

**Scenario:** Resetting per-LOD counters via CPU `buffer_update()` before each compute dispatch adds CPU overhead proportional to cell count.

**Mitigation:** Initialize counters to 0 once. In the compute shader, use atomic exchange or rely on the fact that each thread that survives compaction writes a slot. OR: double-buffer the counters and toggle which half is read/written each frame (no CPU reset needed).

### Issue 6: Memory Budget

| Buffer | Size for 200K entities | Notes |
|---|---|---|
| Entity state (10 vec4s × 200K) | ~12.8 MB | ECS managed (pos_vel + rot_scale + color_extra + uniforms + mesh_id + flags) |
| Camera data | ~256 bytes | One per frame |
| MultiMesh transforms (3 vec4s × 65536 × 8 × 25) | ~48 MB | 25 cells × 8 MM × 3 vec4s × 64K × 16 bytes — **40% less** than stride-5 |
| Extra data SSBO (UNIFORM_STRIDE vec4s × 65536 × 8 × 25) | ~0.8 MB × UNIFORM_STRIDE | Per-MultiMesh, same count as transforms |
| Indirect cmd (5 uint32s × surfaces × 8 × 25) | ~32 KB | Negligible |
| Counters | ~256 bytes per cell | Negligible |
| **Total estimated** | **~60-80 MB** | Well within budgets |

### Issue 7: Instance Indexing in Shaders

The vertex shader reads `gl_InstanceIndex` and uses it to index into the transforms buffer. When using indirect draw, `gl_InstanceIndex` ranges from 0 to `instanceCount - 1` (as set in the indirect command buffer). Since our culling shader compacts visible entities into contiguous slots starting at 0, this maps correctly — visible instance i is at `i * stride` in the transforms buffer.

---

## Appendix: Verify the Approach (Minimal Test)

### Test: Single Mesh, No LOD, Single Cell

```cpp
// 1. Create a simple MultiMesh
RID mm = RS::get_singleton()->multimesh_create();
RS::get_singleton()->multimesh_allocate_data(mm, 1000,
    RS::MULTIMESH_TRANSFORM_3D, false, false, true);
RS::get_singleton()->multimesh_set_mesh(mm, my_debug_mesh_rid);
RS::get_singleton()->multimesh_set_custom_aabb(mm, AABB(-50, -50, -50, 100, 100, 100));

// 2. Get buffer RIDs
RID xform_buf = RS::get_singleton()->multimesh_get_buffer_rd_rid(mm);
RID cmd_buf = RS::get_singleton()->multimesh_get_command_buffer_rd_rid(mm);

// 3. Write transforms via compute (your culling shader)
//    Dispatch a compute shader that writes 100 random transforms to xform_buf
//    and sets cmd_buf[1] = 100 (instance count)

// 4. Create a scene instance
RID instance = RS::get_singleton()->instance_create2(mm, scenario);
RS::get_singleton()->instance_set_transform(instance, Transform3D());

// 5. Verify: you should see 100 entities rendered with full lighting
```

This test proves the pipeline works before scaling to millions of entities.

---

## Summary of Required GDExtension Work

| Component | Effort | Dependencies |
|---|---|---|
| Grid cell management | Medium | None |
| MultiMesh creation + buffer RID caching | Low | None |
| ECS GPU buffer management | Medium | RD API |
| Compute shader: culling + LOD + compaction | High | Requires GLSL knowledge |
| Compute shader: indirect buffer update | Low | Output of culling pass |
| CompositorEffect integration | Medium | Callback setup |
| Camera data upload (per frame) | Low | None |
| Per-instance uniforms (Plan B: Set 2 B1 SSBO) | Low (~20 LOC engine patch) | Add B1 to inc.glsl + `multimesh_get_3d_uniform_set` + expose setter on RS |
| Per-instance uniforms (fallback: custom_data → texture) | Low | RS API + `texelFetch` in material shader |
| Motion vectors (if needed) | Medium | Requires extra compute pass |
| Debug visualization | Medium | Optional |
