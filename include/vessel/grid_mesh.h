// Petter Strandmark 2013.
#ifndef VESSEL_GRID_MESH_H
#define VESSEL_GRID_MESH_H

#include <algorithm>

#include <vessel/mesh.h>

namespace vessel {

// Creating a regular Mesh is slow using only the functions provided
// by Mesh. This class speeds up the creation if all points are in a
// regular, square grid.
class GridMesh :
	public Mesh
{
public:

	// For delayed initialization.
	GridMesh() { }

	GridMesh(int x_dim, int y_dim, double dmax, bool create_pairs = false)
	{
		initialize(x_dim, y_dim, 1, dmax, 0, create_pairs);
	}

	GridMesh(int x_dim, int y_dim ,int z_dim, double dmax, bool create_pairs = false)
	{
		initialize(x_dim, y_dim, z_dim, dmax, 0, create_pairs);
	}

	GridMesh(int x_dim, int y_dim ,int z_dim, double dmax,
	         const std::function<bool(float,float,float,float,float,float)>& ignore,
	         bool create_pairs = false)
	{
		initialize(x_dim, y_dim, z_dim, dmax, &ignore, create_pairs);
	}

	void initialize(int x_dim, int y_dim ,int z_dim, double dmax,
	                const std::function<bool(float,float,float,float,float,float)>* ignore,
	                bool create_pairs);
};


}  // namespace vessel

#endif