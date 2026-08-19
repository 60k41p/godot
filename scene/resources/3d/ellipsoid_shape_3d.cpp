/**************************************************************************/
/*  ellipsoid_shape_3d.cpp                                                */
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

#include "ellipsoid_shape_3d.h"

#include "core/object/class_db.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "servers/physics_3d/physics_server_3d.h"

Vector<Vector3> EllipsoidShape3D::get_debug_mesh_lines() const {
	Vector<Vector3> points;

	for (int i = 0; i <= 360; i++) {
		float ra = Math::deg_to_rad((float)i);
		float rb = Math::deg_to_rad((float)i + 1);
		Point2 a = Vector2(Math::sin(ra), Math::cos(ra));
		Point2 b = Vector2(Math::sin(rb), Math::cos(rb));

		points.push_back(Vector3(a.x, 0, a.y) * radii);
		points.push_back(Vector3(b.x, 0, b.y) * radii);
		points.push_back(Vector3(0, a.x, a.y) * radii);
		points.push_back(Vector3(0, b.x, b.y) * radii);
		points.push_back(Vector3(a.x, a.y, 0) * radii);
		points.push_back(Vector3(b.x, b.y, 0) * radii);
	}

	return points;
}

Ref<ArrayMesh> EllipsoidShape3D::get_debug_arraymesh_faces(const Color &p_modulate) const {
	Array ellipsoid_array;
	ellipsoid_array.resize(RSE::ARRAY_MAX);
	SphereMesh::create_mesh_array(ellipsoid_array, 1.0, 2.0, 32);

	PackedVector3Array verts = ellipsoid_array[RSE::ARRAY_VERTEX];
	for (Vector3 &vertex : verts) {
		vertex *= radii;
	}
	ellipsoid_array[RSE::ARRAY_VERTEX] = verts;

	Vector<Color> colors;
	const int32_t verts_size = verts.size();
	for (int i = 0; i < verts_size; i++) {
		colors.append(p_modulate);
	}

	Ref<ArrayMesh> ellipsoid_mesh = memnew(ArrayMesh);
	ellipsoid_array[RSE::ARRAY_COLOR] = colors;
	ellipsoid_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, ellipsoid_array);
	return ellipsoid_mesh;
}

real_t EllipsoidShape3D::get_enclosing_radius() const {
	return radii.length();
}

void EllipsoidShape3D::_update_shape() {
	PhysicsServer3D::get_singleton()->shape_set_data(get_shape(), radii);
	Shape3D::_update_shape();
}

void EllipsoidShape3D::set_radii(const Vector3 &p_radii) {
	ERR_FAIL_COND_MSG(p_radii.x <= 0 || p_radii.y <= 0 || p_radii.z <= 0, "EllipsoidShape3D radii must be greater than 0.");
	radii = p_radii;
	_update_shape();
	emit_changed();
}

Vector3 EllipsoidShape3D::get_radii() const {
	return radii;
}

void EllipsoidShape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radii", "radii"), &EllipsoidShape3D::set_radii);
	ClassDB::bind_method(D_METHOD("get_radii"), &EllipsoidShape3D::get_radii);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "radii", PROPERTY_HINT_RANGE, "0.001,100,0.001,or_greater,suffix:m"), "set_radii", "get_radii");
}

EllipsoidShape3D::EllipsoidShape3D() :
		Shape3D(PhysicsServer3D::get_singleton()->shape_create(PhysicsServer3D::SHAPE_ELLIPSOID)) {
	set_radii(Vector3(0.5, 0.5, 0.5));
}
