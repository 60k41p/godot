# `multimesh_set_extra_data_rd_rid` — Usage Guide

## Overview

`RenderingServer.multimesh_set_extra_data_rd_rid(multimesh, buffer)` registers an RD storage buffer as **Set 2, Binding 1** in the MultiMesh uniform set. This buffer is accessible from forward clustered and forward mobile shaders via the `INSTANCE_EXTRA` built-in, providing per-instance extra data (color tint, custom uniforms, etc.) with no CPU round-trip.

## API

```gdscript
RenderingServer.multimesh_set_extra_data_rd_rid(multimesh: RID, buffer: RID)
RenderingServer.multimesh_set_extra_data_stride(multimesh: RID, stride: int)
```

- `multimesh` — a MultiMesh created via `RenderingServer.multimesh_create()` (typically allocated with `use_indirect = true` for GPU-driven rendering; non-indirect also works).
- `buffer` — an RD storage buffer created via `RenderingDevice.storage_buffer_create()`. Must have at least `instance_count * stride * 16` bytes.
- `stride` — number of `vec4` slots per instance in the buffer (default: 1). Controls the step between consecutive instances.

## Shader Access

In any `shader_type spatial` material's `vertex()` function, the extra data buffer is accessed via the built-in `INSTANCE_EXTRA` with explicit index brackets:

```glsl
vec4 slot_0 = INSTANCE_EXTRA[0];    // slot 0
vec4 slot_1 = INSTANCE_EXTRA[1];    // slot 1 when stride >= 2
vec4 slot_N = INSTANCE_EXTRA[N];    // any slot < stride
```

- Bare `INSTANCE_EXTRA` (without brackets) is an alias for slot 0 — kept for backward compatibility with stride = 1 shaders. Prefer explicit `INSTANCE_EXTRA[i]`.
- `INSTANCE_EXTRA[i]` returns slot `i`, where `i < extra_data_stride`.
- The index can be a literal or a variable (`INSTANCE_EXTRA[INSTANCE_ID]`).
- `INSTANCE_ID` maps to the per-draw-call instance index (see Important Notes below).

The engine-internal SSBO declaration is:

```glsl
layout(set = 2, binding = 1, std430) restrict readonly buffer InstanceExtra {
    vec4 data[];
}
instance_extra_ssbo;
```

## Typical Workflow

### 1. Create and allocate the MultiMesh

```gdscript
var mm := RenderingServer.multimesh_create()
RenderingServer.multimesh_allocate_data(mm, instance_count,
    RenderingServer.MULTIMESH_TRANSFORM_3D,
    false,   # use_colors
    false,   # use_custom_data
    true)    # use_indirect — typical for GPU-driven rendering (non-indirect also works)

RenderingServer.multimesh_set_mesh(mm, mesh_rid)
RenderingServer.multimesh_set_custom_aabb(mm, cell_aabb)
```

### 2. Create the extra data buffer

```gdscript
var rd := RenderingDevice.get_singleton()
var extra_buf := rd.storage_buffer_create(instance_count * stride * 16)
```

### 3. Register it with the MultiMesh

```gdscript
RenderingServer.multimesh_set_extra_data_rd_rid(mm, extra_buf)
```

After this call, the next time the MultiMesh is rendered, its uniform set will include `extra_buf` at Set 2 Binding 1.

### 4. Write data from a compute shader

```glsl
layout(set = 1, binding = 1, std430) buffer ExtraOut {
    vec4 data[];
} extra_out;

// In your culling/compaction shader:
uint slot = global_visible_index;  // compacted contiguous slot
extra_out.data[slot * STRIDE + 0] = color_tint;
extra_out.data[slot * STRIDE + 1] = custom_uniform;
```

Where `STRIDE` matches the stride you set via `multimesh_set_extra_data_stride`. For non-indirect rendering (no compaction), `slot` simply equals the instance index.

### 5. Read in a material shader

```glsl
shader_type spatial;

void vertex() {
    vec4 color_tint = INSTANCE_EXTRA[0];    // slot 0 (stride >= 1)
    vec4 custom_val = INSTANCE_EXTRA[1];    // slot 1 (stride >= 2)
    // Dynamic indexing also works:
    for (int i = 0; i < 2; i++) {
        vec4 val = INSTANCE_EXTRA[i];
    }
}
```

## Important Notes

- **`INSTANCE_ID` is a transient index, not a stable entity ID.** `INSTANCE_ID` (mapped from `gl_InstanceIndex`) is the per-draw-call instance index — `0, 1, 2, ..., instance_count-1`. For indirect draws where a compute shader compacts visible instances, it is the compacted index, not the original entity ID. If you need persistent entity lookups, store an entity ID in the extra data buffer at a known slot position.

- **`INSTANCE_EXTRA` is array-indexed.** The compiler desugars `INSTANCE_EXTRA[i]` to `instance_extra_ssbo.data[gl_InstanceIndex * stride + i]`. Bare `INSTANCE_EXTRA` without brackets desugars to slot 0 (`... + 0]`) — a legacy alias kept for stride = 1 shaders; prefer explicit `INSTANCE_EXTRA[0]`.

- **`use_indirect` is optional.** Non-indirect MultiMeshes work fine — the extra data buffer binds and indexes the same way. Indirect mode is typical when a compute shader drives instance culling/compaction.

- **Set only once.** The uniform set is cached. If you call `multimesh_set_extra_data_rd_rid` again with a different buffer, the uniform set is invalidated and rebuilt on the next render.

- **No CPU data_cache pollution.** Since you never call CPU-side write APIs (`instance_set_transform`, `set_buffer`, etc.), the engine's `_update_dirty_multimeshes` skips your buffers entirely.

- **Per-MultiMesh, not per-draw.** The extra data buffer is specific to each MultiMesh, so there is no global indexing conflict across different cells or LOD levels.

- **Forward mobile supported.** The `instance_extra_ssbo` declaration is present in both forward clustered and forward mobile shader includes.

- **Stride control.** Use `multimesh_set_extra_data_stride()` to set the number of `vec4` slots per instance. Default is 1.

- **Read-only access.** The SSBO is declared `restrict readonly buffer` in the engine shaders. The vertex shader cannot write to it, allowing the driver to optimize access on tile-based GPUs.
