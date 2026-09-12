#ifndef VKPT_NRD_MATH_H
#define VKPT_NRD_MATH_H

// Q2RTX maps clip XY to UV with * 0.5 + 0.5. NRD uses D3D screen
// coordinates and negates clip Y first (including in its spatial kernels).
// Negate the entire Y row of the column-major projection, including viewport
// offsets, so NRD reconstructs and projects the same positions as Q2RTX.
// World-space normals, view matrices, and screen-space MVs stay unchanged.
static inline void vkpt_nrd_projection(float output[16], const float input[16])
{
	for (int i = 0; i < 16; i++)
		output[i] = (i % 4 == 1) ? -input[i] : input[i];

	// create_projection_matrix() uses a positive depth translation for ray
	// construction. With +Z forward this puts both clip planes behind the
	// camera, so NRD's DecomposeProjection detects RH and flips the view Z.
	// Rebuild the depth row as a standard LH [0,1] projection, preserving
	// the engine's near/far distances: A = (f+n)/(f-n), B = 2*f*n/(f-n).
	output[10] = (input[10] + 1.0f) * 0.5f;
	output[14] = -input[14] * 0.5f;
}

#endif
