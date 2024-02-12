// ---------------------------------------------------------------------
//
// Copyright (C) 2008 - 2024 by the deal.II authors
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

#ifndef dealii_trilinos_tpetra_solver_templates_h
#define dealii_trilinos_tpetra_solver_templates_h

#include <deal.II/base/config.h>

#include "deal.II/lac/solver_control.h"

#include <string>


#ifdef DEAL_II_TRILINOS_WITH_TPETRA
#  ifdef DEAL_II_TRILINOS_WITH_AMESOS2

#    include "deal.II/base/template_constraints.h"
#    include <deal.II/base/conditional_ostream.h>

#    include <deal.II/lac/trilinos_tpetra_solver_direct.h>


DEAL_II_NAMESPACE_OPEN

namespace LinearAlgebra
{
  namespace TpetraWrappers
  {
    /* ---------------------- SolverDirectBase ------------------------ */

    template <typename Number, typename MemorySpace>
    SolverDirectBase<Number, MemorySpace>::SolverDirectBase(
      SolverControl     &cn,
      const std::string &solver_type,
      const bool         output_solver_details)
      : solver_control(cn)
      , solver_type(solver_type)
      , output_solver_details(output_solver_details)
    {
      AssertThrow(Amesos2::query(solver_type),
                  ExcTrilinosAmesos2SolverUnsupported(solver_type));
    }



    template <typename Number, typename MemorySpace>
    SolverControl &
    SolverDirectBase<Number, MemorySpace>::control() const
    {
      return solver_control;
    }



    template <typename Number, typename MemorySpace>
    void
    SolverDirectBase<Number, MemorySpace>::initialize(
      const SparseMatrix<Number, MemorySpace> &A)
    {
      // First set whether we want to print the solver information to screen or
      // not.
      ConditionalOStream verbose_cout(std::cout, output_solver_details);

      // Next allocate the Amesos2 solver with the concrete solver, if possible.
      solver =
        Amesos2::create<typename SparseMatrix<Number, MemorySpace>::MatrixType,
                        MultiVector>(solver_type, A.trilinos_rcp());

      solver->setParameters(Teuchos::rcpFromRef(parameter_list));
      // Now do the actual factorization, which is a two step procedure.
      // The symbolic factorization determines the structure of the inverse,
      // while the numeric factorization does to actual computation of L and U
      verbose_cout << "Starting symbolic factorization" << std::endl;
      solver->symbolicFactorization();

      verbose_cout << "Starting numeric factorization" << std::endl;
      solver->numericFactorization();
    }



    template <typename Number, typename MemorySpace>
    void
    SolverDirectBase<Number, MemorySpace>::solve(
      Vector<Number, MemorySpace>       &x,
      const Vector<Number, MemorySpace> &b)
    {
      // Assign the empty solution vector
      solver->setX(x.trilinos_rcp());

      // Assign the RHS vector
      solver->setB(b.trilinos_rcp());
      // First set whether we want to print the solver information to screen
      // or not.
      ConditionalOStream verbose_cout(std::cout, output_solver_details);

      verbose_cout << "Starting solve" << std::endl;
      solver->solve();

      // Finally, force the SolverControl object to report convergence
      solver_control.check(0, 0);
      if (solver_control.last_check() != SolverControl::success)
        AssertThrow(false,
                    SolverControl::NoConvergence(solver_control.last_step(),
                                                 solver_control.last_value()));
    }



    template <typename Number, typename MemorySpace>
    void
    SolverDirectBase<Number, MemorySpace>::do_solve()
    {
      // First set whether we want to print the solver information to screen or
      // not.
      ConditionalOStream verbose_cout(std::cout, output_solver_details);

      solver->setParameters(Teuchos::rcpFromRef(parameter_list));
      // Now do the actual factorization, which is a two step procedure.
      // The symbolic factorization determines the structure of the inverse,
      // while the numeric factorization does to actual computation of L and U
      verbose_cout << "Starting symbolic factorization" << std::endl;
      solver->symbolicFactorization();

      verbose_cout << "Starting numeric factorization" << std::endl;
      solver->numericFactorization();

      verbose_cout << "Starting solve" << std::endl;
      solver->solve();

      // Finally, force the SolverControl object to report convergence
      solver_control.check(0, 0);

      if (solver_control.last_check() != SolverControl::success)
        AssertThrow(false,
                    SolverControl::NoConvergence(solver_control.last_step(),
                                                 solver_control.last_value()));
    }



    template <typename Number, typename MemorySpace>
    void
    SolverDirectBase<Number, MemorySpace>::solve(
      const SparseMatrix<Number, MemorySpace> &A,
      Vector<Number, MemorySpace>             &x,
      const Vector<Number, MemorySpace>       &b)
    {
      solver =
        Amesos2::create<typename SparseMatrix<Number, MemorySpace>::MatrixType,
                        MultiVector>(solver_type,
                                     A.trilinos_rcp(),
                                     x.trilinos_rcp(),
                                     b.trilinos_rcp());
      do_solve();
    }



    template <typename Number, typename MemorySpace>
    SolverDirect<Number, MemorySpace>::AdditionalData::AdditionalData(
      const std::string &solver_name,
      const bool         output_solver_details)
      : solver_name(solver_name)
      , output_solver_details(output_solver_details)
    {}



    template <typename Number, typename MemorySpace>
    SolverDirect<Number, MemorySpace>::SolverDirect(SolverControl        &cn,
                                                    const AdditionalData &ad)
      : SolverDirectBase<Number, MemorySpace>(cn,
                                              ad.solver_name,
                                              ad.output_solver_details)
    {}



    template <typename Number, typename MemorySpace>
    void
    SolverDirect<Number, MemorySpace>::set_pararameter_list(
      Teuchos::ParameterList &parameter_list)
    {
      this->parameter_list.setParameters(parameter_list);
    }



    template <typename Number, typename MemorySpace>
    SolverDirectKLU2<Number, MemorySpace>::AdditionalData::AdditionalData(
      const std::string &transpose_mode,
      const bool         symmetric_mode,
      const bool         equilibrate_matrix,
      const std::string &column_permutation,
      const std::string &iterative_refinement,
      const bool         output_solver_details)
      : transpose_mode(transpose_mode)
      , symmetric_mode(symmetric_mode)
      , equilibrate_matrix(equilibrate_matrix)
      , column_permutation(column_permutation)
      , iterative_refinement(iterative_refinement)
      , output_solver_details(output_solver_details)
    {}



    template <typename Number, typename MemorySpace>
    SolverDirectKLU2<Number, MemorySpace>::SolverDirectKLU2(
      SolverControl        &cn,
      const AdditionalData &ad)
      : SolverDirectBase<Number, MemorySpace>(cn,
                                              "KLU2",
                                              ad.output_solver_details)
    {
      this->parameter_list               = Teuchos::ParameterList("Amesos2");
      Teuchos::ParameterList klu2_params = this->parameter_list.sublist("KLU2");
      klu2_params.set("Trans", ad.transpose_mode);
      klu2_params.set("Equil", ad.equilibrate_matrix);
      klu2_params.set("IterRefine", ad.iterative_refinement);
      klu2_params.set("SymmetricMode", ad.symmetric_mode);
      klu2_params.set("ColPerm", ad.column_permutation);
    }

  } // namespace TpetraWrappers
} // namespace LinearAlgebra

DEAL_II_NAMESPACE_CLOSE

#  endif // DEAL_II_TRILINOS_WITH_AMESOS2
#endif   // DEAL_II_TRILINOS_WITH_TPETRA

#endif
