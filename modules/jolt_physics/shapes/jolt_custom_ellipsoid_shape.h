/**************************************************************************/
/*  jolt_custom_ellipsoid_shape.h                                         */
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

#pragma once

#include "jolt_custom_shape_type.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/ConvexShape.h>
#include <Jolt/Physics/PhysicsSettings.h>

class JoltCustomEllipsoidShapeSettings final : public JPH::ConvexShapeSettings {
public:
	JoltCustomEllipsoidShapeSettings() = default;

	JoltCustomEllipsoidShapeSettings(JPH::Vec3Arg p_radii, float p_convex_radius = JPH::cDefaultConvexRadius, const JPH::PhysicsMaterial *p_material = nullptr) :
			JPH::ConvexShapeSettings(p_material),
			radii(p_radii),
			convex_radius(p_convex_radius) {}

	virtual JPH::ShapeSettings::ShapeResult Create() const override;

	JPH::Vec3 radii = JPH::Vec3::sReplicate(0.5f);
	float convex_radius = JPH::cDefaultConvexRadius;
};

class JoltCustomEllipsoidShape final : public JPH::ConvexShape {
	class ErodedEllipsoid;
	class Ellipsoid;

public:
	JPH_OVERRIDE_NEW_DELETE
	JoltCustomEllipsoidShape() :
			JPH::ConvexShape(JoltCustomShapeSubType::ELLIPSOID) {}

	JoltCustomEllipsoidShape(const JoltCustomEllipsoidShapeSettings &p_settings, JPH::Shape::ShapeResult &p_result);

	JPH::Vec3 GetRadii() const { return radii; }

	virtual JPH::AABox GetLocalBounds() const override;

	virtual float GetInnerRadius() const override { return radii.ReduceMin(); }

	virtual JPH::MassProperties GetMassProperties() const override;

	virtual JPH::Vec3 GetSurfaceNormal(const JPH::SubShapeID &p_sub_shape_id, JPH::Vec3Arg p_local_surface_position) const override;

	virtual void GetSupportingFace([[maybe_unused]] const JPH::SubShapeID &p_sub_shape_id, [[maybe_unused]] JPH::Vec3Arg p_direction, [[maybe_unused]] JPH::Vec3Arg p_scale, [[maybe_unused]] JPH::Mat44Arg p_center_of_mass_transform, [[maybe_unused]] JPH::Shape::SupportingFace &p_vertices) const override { /* Hit is always a single point, no point in returning anything */ }

	virtual const JPH::ConvexShape::Support *GetSupportFunction(JPH::ConvexShape::ESupportMode p_mode, JPH::ConvexShape::SupportBuffer &p_buffer, JPH::Vec3Arg p_scale) const override;

	virtual void GetSubmergedVolume(JPH::Mat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, const JPH::Plane &p_surface, float &p_total_volume, float &p_submerged_volume, JPH::Vec3 &p_center_of_buoyancy JPH_IF_DEBUG_RENDERER(, JPH::RVec3Arg p_base_offset)) const override;

#ifdef JPH_DEBUG_RENDERER
	virtual void Draw(JPH::DebugRenderer *p_renderer, JPH::RMat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, JPH::ColorArg p_color, bool p_use_material_colors, bool p_draw_wireframe) const override;
#endif // JPH_DEBUG_RENDERER

	virtual void CollideSoftBodyVertices(JPH::Mat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, const JPH::CollideSoftBodyVertexIterator &p_vertices, JPH::uint p_num_vertices, int p_colliding_shape_index) const override;

	virtual JPH::Shape::Stats GetStats() const override { return JPH::Shape::Stats(sizeof(*this), 0); }

	virtual float GetVolume() const override;

	virtual bool IsValidScale(JPH::Vec3Arg p_scale) const override;

	virtual JPH::Vec3 MakeScaleValid(JPH::Vec3Arg p_scale) const override;

	virtual void SaveBinaryState(JPH::StreamOut &p_stream) const override;

protected:
	virtual void RestoreBinaryState(JPH::StreamIn &p_stream) override;

private:
	JPH::Vec3 GetScaledRadii(JPH::Vec3Arg p_scale) const { return p_scale.Abs() * radii; }

	JPH::Vec3 radii = JPH::Vec3::sReplicate(0.5f);
	float convex_radius = JPH::cDefaultConvexRadius;
};
