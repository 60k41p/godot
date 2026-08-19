/**************************************************************************/
/*  jolt_custom_ellipsoid_shape.cpp                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "jolt_custom_ellipsoid_shape.h"

#include <Jolt/Physics/Collision/CollideSoftBodyVertexIterator.h>
#include <Jolt/Physics/Collision/Shape/ScaleHelpers.h>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRenderer.h>
#endif // JPH_DEBUG_RENDERER

namespace {

JPH::Vec3 get_ellipsoid_support(JPH::Vec3Arg p_radii, JPH::Vec3Arg p_direction) {
	const JPH::Vec3 radii_sq = p_radii * p_radii;
	const float denominator = sqrt(radii_sq.Dot(p_direction * p_direction));
	return denominator > 0.0f ? radii_sq * p_direction / denominator : JPH::Vec3::sZero();
}

} // namespace

class JoltCustomEllipsoidShape::EllipsoidNoConvex final : public JPH::ConvexShape::Support {
public:
	EllipsoidNoConvex(JPH::Vec3Arg p_radii, float p_convex_radius) :
			radii(p_radii),
			convex_radius(p_convex_radius) {
		static_assert(sizeof(EllipsoidNoConvex) <= sizeof(JPH::ConvexShape::SupportBuffer), "Buffer size too small");
		JPH_ASSERT(JPH::IsAligned(this, alignof(EllipsoidNoConvex)));
	}

	virtual JPH::Vec3 GetSupport(JPH::Vec3Arg p_direction) const override {
		return get_ellipsoid_support(radii, p_direction);
	}

	virtual float GetConvexRadius() const override {
		return convex_radius;
	}

private:
	JPH::Vec3 radii = JPH::Vec3::sZero();
	float convex_radius = 0.0f;
};

class JoltCustomEllipsoidShape::EllipsoidWithConvex final : public JPH::ConvexShape::Support {
public:
	EllipsoidWithConvex(JPH::Vec3Arg p_radii, float p_convex_radius) :
			radii(p_radii),
			convex_radius(p_convex_radius) {
		static_assert(sizeof(EllipsoidWithConvex) <= sizeof(JPH::ConvexShape::SupportBuffer), "Buffer size too small");
		JPH_ASSERT(JPH::IsAligned(this, alignof(EllipsoidWithConvex)));
	}

	virtual JPH::Vec3 GetSupport(JPH::Vec3Arg p_direction) const override {
		JPH::Vec3 support = get_ellipsoid_support(radii, p_direction);

		const float len = p_direction.Length();
		if (len > 0.0f) {
			support += (convex_radius / len) * p_direction;
		}

		return support;
	}

	virtual float GetConvexRadius() const override {
		return 0.0f;
	}

private:
	JPH::Vec3 radii = JPH::Vec3::sZero();
	float convex_radius = 0.0f;
};

JPH::ShapeSettings::ShapeResult JoltCustomEllipsoidShapeSettings::Create() const {
	if (radii.GetX() <= 0.0f || radii.GetY() <= 0.0f || radii.GetZ() <= 0.0f || convex_radius < 0.0f || radii.ReduceMin() <= convex_radius) {
		JPH::ShapeSettings::ShapeResult result;
		result.SetError("Invalid radii or convex radius");
		return result;
	}

	JPH::ShapeSettings::ShapeResult result;
	new JoltCustomEllipsoidShape(*this, result);
	return result;
}

JoltCustomEllipsoidShape::JoltCustomEllipsoidShape(const JoltCustomEllipsoidShapeSettings &p_settings, JPH::Shape::ShapeResult &p_result) :
		JPH::ConvexShape(JoltCustomShapeSubType::ELLIPSOID, p_settings, p_result),
		radii(p_settings.radii),
		convex_radius(p_settings.convex_radius) {
	if (radii.GetX() <= 0.0f || radii.GetY() <= 0.0f || radii.GetZ() <= 0.0f || convex_radius < 0.0f || radii.ReduceMin() <= convex_radius) {
		p_result.SetError("Invalid radii or convex radius");
		return;
	}

	p_result.Set(this);
}

JPH::AABox JoltCustomEllipsoidShape::GetLocalBounds() const {
	const JPH::Vec3 extent = radii + JPH::Vec3::sReplicate(convex_radius);
	return JPH::AABox(-extent, extent);
}

JPH::MassProperties JoltCustomEllipsoidShape::GetMassProperties() const {
	JPH::MassProperties p;

	const float rxs = JPH::Square(radii.GetX());
	const float rys = JPH::Square(radii.GetY());
	const float rzs = JPH::Square(radii.GetZ());

	p.mMass = (4.0f / 3.0f * JPH::JPH_PI) * radii.GetX() * radii.GetY() * radii.GetZ() * GetDensity();
	p.mInertia = JPH::Mat44::sScale(JPH::Vec3(0.2f * p.mMass * (rys + rzs), 0.2f * p.mMass * (rxs + rzs), 0.2f * p.mMass * (rxs + rys)));

	return p;
}

JPH::Vec3 JoltCustomEllipsoidShape::GetSurfaceNormal(const JPH::SubShapeID &p_sub_shape_id, JPH::Vec3Arg p_local_surface_position) const {
	JPH_ASSERT(p_sub_shape_id.IsEmpty(), "Invalid subshape ID");

	const JPH::Vec3 normal = p_local_surface_position / (radii * radii);
	const float len = normal.Length();
	return len != 0.0f ? normal / len : JPH::Vec3::sAxisY();
}

const JPH::ConvexShape::Support *JoltCustomEllipsoidShape::GetSupportFunction(JPH::ConvexShape::ESupportMode p_mode, JPH::ConvexShape::SupportBuffer &p_buffer, JPH::Vec3Arg p_scale) const {
	const JPH::Vec3 scaled_radii = GetScaledRadii(p_scale);

	switch (p_mode) {
		case JPH::ConvexShape::ESupportMode::IncludeConvexRadius:
			return new (&p_buffer) EllipsoidWithConvex(scaled_radii, convex_radius);

		case JPH::ConvexShape::ESupportMode::ExcludeConvexRadius:
		case JPH::ConvexShape::ESupportMode::Default:
			return new (&p_buffer) EllipsoidNoConvex(scaled_radii, convex_radius);
	}

	JPH_ASSERT(false);
	return nullptr;
}

void JoltCustomEllipsoidShape::GetSubmergedVolume(JPH::Mat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, const JPH::Plane &p_surface, float &p_total_volume, float &p_submerged_volume, JPH::Vec3 &p_center_of_buoyancy JPH_IF_DEBUG_RENDERER(, JPH::RVec3Arg p_base_offset)) const {
	const JPH::Vec3 scaled_radii = GetScaledRadii(p_scale);
	p_total_volume = (4.0f / 3.0f * JPH::JPH_PI) * scaled_radii.GetX() * scaled_radii.GetY() * scaled_radii.GetZ();

	// Work in the local space of the shape, where it becomes a unit sphere when divided by its radii.
	const JPH::Plane local_surface = p_surface.GetTransformed(p_center_of_mass_transform.InversedRotationTranslation());

	const JPH::Vec3 plane_normal = scaled_radii * local_surface.GetNormal();
	const float plane_normal_len = plane_normal.Length();
	const float distance_to_surface = local_surface.GetConstant() / plane_normal_len;

	if (distance_to_surface >= 1.0f) {
		// Above surface
		p_submerged_volume = 0.0f;
		p_center_of_buoyancy = JPH::Vec3::sZero();
	} else if (distance_to_surface <= -1.0f) {
		// Under surface
		p_submerged_volume = p_total_volume;
		p_center_of_buoyancy = p_center_of_mass_transform.GetTranslation();
	} else {
		// Intersecting surface

		// Calculate submerged volume, see: https://en.wikipedia.org/wiki/Spherical_cap
		const float h = 1.0f - distance_to_surface;
		p_submerged_volume = (JPH::JPH_PI / 3.0f) * JPH::Square(h) * (3.0f - h) * scaled_radii.GetX() * scaled_radii.GetY() * scaled_radii.GetZ();

		// Calculate center of buoyancy, see: http://mathworld.wolfram.com/SphericalCap.html (eq 10)
		const float z = (3.0f / 4.0f) * JPH::Square(2.0f - h) / (3.0f - h);
		const JPH::Vec3 center_of_buoyancy = -(z / plane_normal_len) * plane_normal;
		p_center_of_buoyancy = p_center_of_mass_transform * (scaled_radii * center_of_buoyancy);
	}

#ifdef JPH_DEBUG_RENDERER
	// Draw center of buoyancy
	if (sDrawSubmergedVolumes) {
		JPH::DebugRenderer::sInstance->DrawWireSphere(p_base_offset + p_center_of_buoyancy, 0.05f, JPH::Color::sRed, 1);
	}
#endif // JPH_DEBUG_RENDERER
}

#ifdef JPH_DEBUG_RENDERER
void JoltCustomEllipsoidShape::Draw(JPH::DebugRenderer *p_renderer, JPH::RMat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, JPH::ColorArg p_color, bool p_use_material_colors, bool p_draw_wireframe) const {
	const JPH::DebugRenderer::EDrawMode draw_mode = p_draw_wireframe ? JPH::DebugRenderer::EDrawMode::Wireframe : JPH::DebugRenderer::EDrawMode::Solid;
	p_renderer->DrawUnitSphere(p_center_of_mass_transform * JPH::Mat44::sScale(GetScaledRadii(p_scale)), p_use_material_colors ? GetMaterial()->GetDebugColor() : p_color, JPH::DebugRenderer::ECastShadow::On, draw_mode);
}
#endif // JPH_DEBUG_RENDERER

void JoltCustomEllipsoidShape::CollideSoftBodyVertices(JPH::Mat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, const JPH::CollideSoftBodyVertexIterator &p_vertices, JPH::uint p_num_vertices, int p_colliding_shape_index) const {
	const JPH::Mat44 inverse_transform = p_center_of_mass_transform.InversedRotationTranslation();
	const JPH::Vec3 scaled_radii = GetScaledRadii(p_scale);

	for (JPH::CollideSoftBodyVertexIterator v = p_vertices, sbv_end = p_vertices + p_num_vertices; v != sbv_end; ++v) {
		if (v.GetInvMass() > 0.0f) {
			const JPH::Vec3 local_pos = inverse_transform * v.GetPosition();
			const JPH::Vec3 unit_pos = local_pos / scaled_radii;
			const float unit_distance = unit_pos.Length();

			if (unit_distance <= 1.0f) {
				const JPH::Vec3 unit_normal = unit_distance > 0.0f ? unit_pos / unit_distance : JPH::Vec3::sAxisY();
				const JPH::Vec3 local_closest = scaled_radii * unit_normal;

				const JPH::Vec3 delta = local_pos - local_closest;
				const float penetration = delta.Length();

				if (v.UpdatePenetration(penetration)) {
					const JPH::Vec3 normal = penetration > 0.0f ? delta / penetration : JPH::Vec3::sAxisY();

					v.SetCollision(JPH::Plane::sFromPointAndNormal(local_closest, normal).GetTransformed(p_center_of_mass_transform), p_colliding_shape_index);
				}
			}
		}
	}
}

float JoltCustomEllipsoidShape::GetVolume() const {
	return (4.0f / 3.0f * JPH::JPH_PI) * radii.GetX() * radii.GetY() * radii.GetZ();
}

bool JoltCustomEllipsoidShape::IsValidScale(JPH::Vec3Arg p_scale) const {
	return !JPH::ScaleHelpers::IsZeroScale(p_scale);
}

JPH::Vec3 JoltCustomEllipsoidShape::MakeScaleValid(JPH::Vec3Arg p_scale) const {
	return JPH::ScaleHelpers::MakeNonZeroScale(p_scale);
}

void JoltCustomEllipsoidShape::SaveBinaryState(JPH::StreamOut &p_stream) const {
	JPH::ConvexShape::SaveBinaryState(p_stream);

	p_stream.Write(radii);
	p_stream.Write(convex_radius);
}

void JoltCustomEllipsoidShape::RestoreBinaryState(JPH::StreamIn &p_stream) {
	JPH::ConvexShape::RestoreBinaryState(p_stream);

	p_stream.Read(radii);
	p_stream.Read(convex_radius);
}
