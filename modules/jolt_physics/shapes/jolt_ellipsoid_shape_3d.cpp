/**************************************************************************/
/*  jolt_ellipsoid_shape_3d.cpp                                           */
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

#include "jolt_ellipsoid_shape_3d.h"

#include "../jolt_project_settings.h"
#include "../misc/jolt_type_conversions.h"
#include "jolt_custom_ellipsoid_shape.h"

JPH::ShapeRefC JoltEllipsoidShape3D::_build() const {
	ERR_FAIL_COND_V_MSG(radii.x <= 0.0f || radii.y <= 0.0f || radii.z <= 0.0f, nullptr, vformat("Failed to build Jolt Physics ellipsoid shape with %s. Its radii must all be greater than 0. This shape belongs to %s.", to_string(), _owners_to_string()));

	const float min_radius = (float)radii[radii.min_axis_index()];
	const float actual_margin = MIN(margin, min_radius * JoltProjectSettings::collision_margin_fraction);

	const JoltCustomEllipsoidShapeSettings shape_settings(to_jolt(radii), actual_margin);
	const JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
	ERR_FAIL_COND_V_MSG(shape_result.HasError(), nullptr, vformat("Failed to build Jolt Physics ellipsoid shape with %s. It returned the following error: '%s'. This shape belongs to %s.", to_string(), to_godot(shape_result.GetError()), _owners_to_string()));

	return shape_result.Get();
}

Variant JoltEllipsoidShape3D::get_data() const {
	return radii;
}

void JoltEllipsoidShape3D::set_data(const Variant &p_data) {
	ERR_FAIL_COND(p_data.get_type() != Variant::VECTOR3);

	const Vector3 new_radii = p_data;
	if (unlikely(new_radii == radii)) {
		return;
	}

	radii = new_radii;

	destroy();
}

void JoltEllipsoidShape3D::set_margin(float p_margin) {
	if (unlikely(margin == p_margin)) {
		return;
	}

	margin = p_margin;

	destroy();
}

AABB JoltEllipsoidShape3D::get_aabb() const {
	return AABB(-radii, radii * 2.0f);
}

String JoltEllipsoidShape3D::to_string() const {
	return vformat("{radii=%v margin=%f}", radii, margin);
}
