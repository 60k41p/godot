# `multimesh_set_extra_data_rd_rid` — Usage Guide

## Overview

`RenderingServer.multimesh_set_extra_data_rd_rid(multimesh, buffer)` registers an RD storage buffer as **Set 2, Binding 1** in the MultiMesh uniform set. This buffer is accessible from forward clustered and forward mobile shaders at `gl_InstanceIndex`, providing per-instance extra data (color tint, custom uniforms, etc.) with no CPU round-trip.

## API

```gdscript
RenderingServer.multimesh_set_extra_data_rd_rid(multimesh: RID, buffer: RID)
```

- `multimesh` — a MultiMesh created via `RenderingServer.multimesh_create()` and allocated with `use_indirect = true`.
- `buffer` — an RD storage buffer created via `RenderingDevice.storage_buffer_create()`. Must have at least `instance_count * unif_stride * 16` bytes.

## Shader Access

In any `shader_type spatial` material, the extra data is available at:

```glsl
// `instance_extra.data[]` is indexed by gl_InstanceIndex
vec4 my_value = instance_extra.data[gl_InstanceIndex * UNIFORM_STRIDE + slot];
```

The GLSL declaration (engine-internal) is:

```glsl
layout(set = 2, binding = 1, std430) restrict buffer InstanceExtra {
    vec4 data[];
}
instance_extra;
```

## Typical Workflow

### 1. Create and allocate the MultiMesh

```gdscript
var mm := RenderingServer.multimesh_create()
RenderingServer.multimesh_allocate_data(mm, instance_count,
    RenderingServer.MULTIMESH_TRANSFORM_3D,
    false,   # use_colors
    false,   # use_custom_data
    true)    # use_indirect — REQUIRED for GPU-driven rendering

RenderingServer.multimesh_set_mesh(mm, mesh_rid)
RenderingServer.multimesh_set_custom_aabb(mm, cell_aabb)
```

### 2. Create the extra data buffer

```gdscript
var rd := RenderingDevice.get_singleton()
var extra_buf := rd.storage_buffer_create(instance_count * UNIFORM_STRIDE * 16)
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
extra_out.data[slot * UNIFORM_STRIDE + 0] = color_tint;
extra_out.data[slot * UNIFORM_STRIDE + 1] = custom_uniform;
```

### 5. Read in a material shader

```glsl
shader_type spatial;

void vertex() {
    vec4 color_tint = instance_extra.data[gl_InstanceIndex * 2 + 0];
    vec4 custom     = instance_extra.data[gl_InstanceIndex * 2 + 1];
    // ...
}
```

## Important Notes

- **Set only once.** The uniform set is cached. If you call `multimesh_set_extra_data_rd_rid` again with a different buffer, the uniform set is invalidated and rebuilt on the next render.
- **Compatible with indirect MultiMesh.** Works with `use_indirect = true`. Your compute shader writes visible instance transforms to the transforms SSBO (Set 2 B0) and extra data to this buffer (Set 2 B1), both compacted by `gl_InstanceIndex`.
- **No CPU data_cache pollution.** Since you never call CPU-side write APIs (`instance_set_transform`, `set_buffer`, etc.), the engine's `_update_dirty_multimeshes` skips your buffers entirely.
- **Per-MultiMesh, not per-draw.** The extra data buffer is specific to each MultiMesh, so there is no global indexing conflict across different cells or LOD levels.
- **Forward mobile supported.** The `instance_extra` declaration is present in both forward clustered and forward mobile shader includes.
