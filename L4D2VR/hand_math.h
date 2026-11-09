#pragma once
#include <cmath>

// Small math library for bones. Doesn't depend on the game, so it can be tested on its own (tests/).
namespace HandMath
{
	struct Vec3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;

		Vec3() {}
		Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

		Vec3 operator+(const Vec3 &v) const { return Vec3(x + v.x, y + v.y, z + v.z); }
		Vec3 operator-(const Vec3 &v) const { return Vec3(x - v.x, y - v.y, z - v.z); }
		Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
	};

	inline float Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline Vec3 Cross(const Vec3 &a, const Vec3 &b) { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
	inline float Length(const Vec3 &v) { return sqrtf(Dot(v, v)); }

	inline Vec3 Normalized(const Vec3 &v, const Vec3 &fallback = Vec3(1.0f, 0.0f, 0.0f))
	{
		const float length = Length(v);
		return length > 1e-6f ? v * (1.0f / length) : fallback;
	}

	// Same memory layout as matrix3x4_t: the columns are the x, y and z axes and the position
	struct Mat34
	{
		float m[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
	};

	inline Vec3 Axis(const Mat34 &a, int column) { return Vec3(a.m[0][column], a.m[1][column], a.m[2][column]); }
	inline Vec3 Origin(const Mat34 &a) { return Axis(a, 3); }

	inline void SetAxis(Mat34 &a, int column, const Vec3 &v)
	{
		a.m[0][column] = v.x;
		a.m[1][column] = v.y;
		a.m[2][column] = v.z;
	}

	inline Mat34 FromAxes(const Vec3 &x, const Vec3 &y, const Vec3 &z, const Vec3 &origin = Vec3())
	{
		Mat34 r;
		SetAxis(r, 0, x);
		SetAxis(r, 1, y);
		SetAxis(r, 2, z);
		SetAxis(r, 3, origin);
		return r;
	}

	inline Vec3 RotateVector(const Mat34 &a, const Vec3 &v)
	{
		return Vec3(a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
			a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
			a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z);
	}

	inline Vec3 TransformPoint(const Mat34 &a, const Vec3 &v) { return RotateVector(a, v) + Origin(a); }

	inline Mat34 Multiply(const Mat34 &a, const Mat34 &b)
	{
		Mat34 r;
		for (int row = 0; row < 3; ++row)
		{
			for (int column = 0; column < 3; ++column)
				r.m[row][column] = a.m[row][0] * b.m[0][column] + a.m[row][1] * b.m[1][column] + a.m[row][2] * b.m[2][column];
			r.m[row][3] = a.m[row][0] * b.m[0][3] + a.m[row][1] * b.m[1][3] + a.m[row][2] * b.m[2][3] + a.m[row][3];
		}
		return r;
	}

	// For matrices with only a rotation and a position
	inline Mat34 InvertRigid(const Mat34 &a)
	{
		Mat34 r;
		for (int row = 0; row < 3; ++row)
			for (int column = 0; column < 3; ++column)
				r.m[row][column] = a.m[column][row];

		const Vec3 inverted = RotateVector(r, Origin(a));
		SetAxis(r, 3, inverted * -1.0f);
		return r;
	}

	// Inverse of the rotation only, the position is cleared
	inline Mat34 TransposeRotation(const Mat34 &a)
	{
		Mat34 r = InvertRigid(a);
		SetAxis(r, 3, Vec3());
		return r;
	}

	inline Mat34 AxisAngle(const Vec3 &axisIn, float radians)
	{
		const Vec3 a = Normalized(axisIn);
		const float s = sinf(radians), c = cosf(radians), t = 1.0f - c;

		Mat34 r;
		r.m[0][0] = t * a.x * a.x + c;       r.m[0][1] = t * a.x * a.y - s * a.z; r.m[0][2] = t * a.x * a.z + s * a.y;
		r.m[1][0] = t * a.x * a.y + s * a.z; r.m[1][1] = t * a.y * a.y + c;       r.m[1][2] = t * a.y * a.z - s * a.x;
		r.m[2][0] = t * a.x * a.z - s * a.y; r.m[2][1] = t * a.y * a.z + s * a.x; r.m[2][2] = t * a.z * a.z + c;
		return r;
	}

	// Scales the rotation to nothing: every vertex skinned to this bone ends up at the position.
	inline Mat34 CollapsedAt(const Vec3 &position)
	{
		Mat34 r = FromAxes(Vec3(), Vec3(), Vec3(), position);
		return r;
	}

	inline Vec3 Lerp(const Vec3 &a, const Vec3 &b, float t) { return a + (b - a) * t; }

	// Moves from a to b. Good for short transitions, the result is kept a proper rotation.
	inline Mat34 Blend(const Mat34 &a, const Mat34 &b, float t)
	{
		const Vec3 x = Normalized(Lerp(Axis(a, 0), Axis(b, 0), t), Axis(b, 0));
		Vec3 y = Lerp(Axis(a, 1), Axis(b, 1), t);
		y = Normalized(y - x * Dot(x, y), Axis(b, 1));
		return FromAxes(x, y, Cross(x, y), Lerp(Origin(a), Origin(b), t));
	}

	inline bool IsFinite(const Mat34 &a)
	{
		for (int row = 0; row < 3; ++row)
			for (int column = 0; column < 4; ++column)
				if (!std::isfinite(a.m[row][column]))
					return false;
		return true;
	}
}
