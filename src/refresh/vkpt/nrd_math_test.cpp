#include "nrd_math.h"

#include <cmath>
#include <cstdio>
#include "ml.h"

static void project(const float matrix[16], const float point[4], bool nrd, float uv[2])
{
	float clip[4] = {};
	for (int row = 0; row < 4; row++)
		for (int col = 0; col < 4; col++)
			clip[row] += matrix[col * 4 + row] * point[col];
	uv[0] = clip[0] / clip[3] * 0.5f + 0.5f;
	uv[1] = clip[1] / clip[3] * (nrd ? -0.5f : 0.5f) + 0.5f;
}

int main()
{
	// Include asymmetric viewports and Y translation, not just the usual
	// centered perspective matrix. Test different current/previous cameras.
	const float projections[][16] = {
		{1.2f, 0, 0, 0, 0, -1.8f, 0, 0, 0, 0, 1.01f, 1, 0, 0, 2, 0},
		{0.8f, 0, 0, 0, 0, -1.1f, 0, 0, 0.2f, -0.3f, 1.01f, 1, 0, 0.1f, 2, 0}
	};
	for (const auto& projection : projections) {
		float converted[16];
		vkpt_nrd_projection(converted, projection);
		// Exercise the same decomposition NRD uses to decide whether to flip
		// worldToView's Z axis. A UV-only round trip misses this failure.
		float4x4 matrix(float4(converted), float4(converted + 4),
			float4(converted + 8), float4(converted + 12));
		uint32_t flags = 0;
		DecomposeProjection(STYLE_D3D, STYLE_D3D, matrix, &flags,
			nullptr, nullptr, nullptr, nullptr, nullptr);
		if (!(flags & PROJ_LEFT_HANDED)) {
			std::fprintf(stderr, "NRD incorrectly detects a right-handed camera\n");
			return 1;
		}
		const float near_z = projection[14] / (projection[10] + 1.0f);
		const float far_z = projection[14] / (projection[10] - 1.0f);
		const float near_depth = converted[10] + converted[14] / near_z;
		const float far_depth = converted[10] + converted[14] / far_z;
		if (std::fabs(near_depth) > 1e-6f || std::fabs(far_depth - 1.0f) > 1e-6f) {
			std::fprintf(stderr, "NRD clip depth does not span [0,1]\n");
			return 1;
		}
		for (int x = -3; x <= 3; x++) {
			for (int y = -3; y <= 3; y++) {
				for (int z = 4; z <= 20; z += 4) {
					const float point[4] = {float(x), float(y), float(z), 1};
					float engine_uv[2], nrd_uv[2];
					project(projection, point, false, engine_uv);
					project(converted, point, true, nrd_uv);
					for (int axis = 0; axis < 2; axis++) {
						if (std::fabs(engine_uv[axis] - nrd_uv[axis]) > 1e-6f) {
							std::fprintf(stderr, "NRD projection disagrees on axis %d\n", axis);
							return 1;
						}
					}
					// Reconstruct view Y using NRD's top-left UV convention.
					const float reconstructed_y = ((1 - 2 * engine_uv[1]) * z
						- converted[9] * z - converted[13]) / converted[5];
					if (std::fabs(reconstructed_y - y) > 1e-5f)
						return 1;
				}
			}
		}
	}
	return 0;
}
