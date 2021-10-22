// ---------------------------------------------------------------------
//
// Copyright (C) 2008 - 2020 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------



// Test interaction with p4est with a few simple coarse grids in 2d

#include <deal.II/base/tensor.h>

#include <deal.II/distributed/tria.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_out.h>
#include <deal.II/grid/tria.h>

#include "../tests.h"

#include "coarse_grid_common.h"



template <int dim>
void
test(std::ostream & /*out*/)
{
  if (true)
    {
      deallog << "hyper_cube" << std::endl;

      parallel::distributed::Triangulation<dim> tr(
        MPI_COMM_WORLD,
        Triangulation<dim>::none,
        parallel::distributed::Triangulation<
          dim>::communicate_vertices_to_p4est);

      GridGenerator::hyper_cube(tr);
      write_vtk(tr, "1");
    }


  if (true)
    {
      deallog << "hyper_ball" << std::endl;

      parallel::distributed::Triangulation<dim> tr(
        MPI_COMM_WORLD,
        Triangulation<dim>::none,
        parallel::distributed::Triangulation<
          dim>::communicate_vertices_to_p4est);

      GridGenerator::hyper_ball(tr, Point<dim>(), 3.);
      write_vtk(tr, "2");
    }

  if (true)
    {
      deallog << "half_hyper_ball" << std::endl;

      parallel::distributed::Triangulation<dim> tr(
        MPI_COMM_WORLD,
        Triangulation<dim>::none,
        parallel::distributed::Triangulation<
          dim>::communicate_vertices_to_p4est);

      GridGenerator::half_hyper_ball(tr, Point<dim>(), 3.);
      write_vtk(tr, "3");
    }
}


int
main(int argc, char *argv[])
{
  initlog();
  Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, 1);

  deallog.push("2d");
  test<2>(deallog.get_file_stream());
  deallog.pop();
}
