/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2003 - 2020 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE.md at
 * the top level directory of deal.II.
 *
 * ---------------------------------------------------------------------
 */

// compare serial version of step-16 to parallel version of step-50
// using the same conditions: smoothers, solvers, marking strategy,
// RHS, coefficient, domain, etc

#include <deal.II/base/function.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/utilities.h>

#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_values.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/grid/manifold_lib.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>

#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/vector.h>

#include <deal.II/multigrid/mg_coarse.h>
#include <deal.II/multigrid/mg_matrix.h>
#include <deal.II/multigrid/mg_smoother.h>
#include <deal.II/multigrid/mg_tools.h>
#include <deal.II/multigrid/mg_transfer.h>
#include <deal.II/multigrid/multigrid.h>

#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/numerics/vector_tools.h>

#include <sstream>

#include "../tests.h"


template <int dim>
class LaplaceProblem
{
public:
  LaplaceProblem(const unsigned int deg);
  void
  run();

private:
  void
  setup_system();
  void
  assemble_system();
  void
  assemble_multigrid();
  void
  solve();
  void
  refine_grid();
  void
  output_results(const unsigned int cycle) const;

  Triangulation<dim> triangulation;
  FE_Q<dim>          fe;
  DoFHandler<dim>    mg_dof_handler;

  SparsityPattern      sparsity_pattern;
  SparseMatrix<double> system_matrix;

  AffineConstraints<double> constraints;

  Vector<double> solution;
  Vector<double> system_rhs;

  const unsigned int degree;

  MGLevelObject<SparsityPattern>      mg_sparsity_patterns;
  MGLevelObject<SparseMatrix<double>> mg_matrices;
  MGLevelObject<SparseMatrix<double>> mg_interface_matrices;
  MGConstrainedDoFs                   mg_constrained_dofs;
};


template <int dim>
class Coefficient : public Function<dim>
{
public:
  Coefficient()
    : Function<dim>()
  {}

  virtual double
  value(const Point<dim> &p, const unsigned int component = 0) const;

  virtual void
  value_list(const std::vector<Point<dim>> &points,
             std::vector<double> &          values,
             const unsigned int             component = 0) const;
};



template <int dim>
double
Coefficient<dim>::value(const Point<dim> &p, const unsigned int) const
{
  if (p.square() < 0.5 * 0.5)
    return 5;
  else
    return 1;
}



template <int dim>
void
Coefficient<dim>::value_list(const std::vector<Point<dim>> &points,
                             std::vector<double> &          values,
                             const unsigned int             component) const
{
  const unsigned int n_points = points.size();

  Assert(values.size() == n_points,
         ExcDimensionMismatch(values.size(), n_points));

  Assert(component == 0, ExcIndexRange(component, 0, 1));

  for (unsigned int i = 0; i < n_points; ++i)
    values[i] = Coefficient<dim>::value(points[i]);
}


template <int dim>
LaplaceProblem<dim>::LaplaceProblem(const unsigned int degree)
  : triangulation(Triangulation<dim>::limit_level_difference_at_vertices)
  , fe(degree)
  , mg_dof_handler(triangulation)
  , degree(degree)
{}


template <int dim>
void
LaplaceProblem<dim>::setup_system()
{
  mg_dof_handler.distribute_dofs(fe);
  mg_dof_handler.distribute_mg_dofs();

  sparsity_pattern.reinit(mg_dof_handler.n_dofs(),
                          mg_dof_handler.n_dofs(),
                          mg_dof_handler.max_couplings_between_dofs());
  DoFTools::make_sparsity_pattern(
    static_cast<const DoFHandler<dim> &>(mg_dof_handler), sparsity_pattern);

  solution.reinit(mg_dof_handler.n_dofs());
  system_rhs.reinit(mg_dof_handler.n_dofs());

  constraints.clear();
  DoFTools::make_hanging_node_constraints(mg_dof_handler, constraints);
  std::map<types::boundary_id, const Function<dim> *> dirichlet_boundary;
  Functions::ZeroFunction<dim> homogeneous_dirichlet_bc(1);
  dirichlet_boundary[0] = &homogeneous_dirichlet_bc;
  MappingQGeneric<dim> mapping(1);
  VectorTools::interpolate_boundary_values(mapping,
                                           mg_dof_handler,
                                           dirichlet_boundary,
                                           constraints);
  constraints.close();
  constraints.condense(sparsity_pattern);
  sparsity_pattern.compress();
  system_matrix.reinit(sparsity_pattern);

  mg_constrained_dofs.clear();
  mg_constrained_dofs.initialize(mg_dof_handler, dirichlet_boundary);
  const unsigned int n_levels = triangulation.n_levels();

  mg_interface_matrices.resize(0, n_levels - 1);
  mg_interface_matrices.clear_elements();
  mg_matrices.resize(0, n_levels - 1);
  mg_matrices.clear_elements();
  mg_sparsity_patterns.resize(0, n_levels - 1);

  for (unsigned int level = 0; level < n_levels; ++level)
    {
      DynamicSparsityPattern csp;
      csp.reinit(mg_dof_handler.n_dofs(level), mg_dof_handler.n_dofs(level));
      MGTools::make_sparsity_pattern(mg_dof_handler, csp, level);

      mg_sparsity_patterns[level].copy_from(csp);

      mg_matrices[level].reinit(mg_sparsity_patterns[level]);
      mg_interface_matrices[level].reinit(mg_sparsity_patterns[level]);
    }
}


template <int dim>
void
LaplaceProblem<dim>::assemble_system()
{
  const QGauss<dim> quadrature_formula(degree + 1);

  FEValues<dim> fe_values(fe,
                          quadrature_formula,
                          update_values | update_gradients |
                            update_quadrature_points | update_JxW_values);

  const unsigned int dofs_per_cell = fe.dofs_per_cell;
  const unsigned int n_q_points    = quadrature_formula.size();

  FullMatrix<double> cell_matrix(dofs_per_cell, dofs_per_cell);
  Vector<double>     cell_rhs(dofs_per_cell);

  std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

  const Coefficient<dim> coefficient;
  std::vector<double>    coefficient_values(n_q_points);

  typename DoFHandler<dim>::active_cell_iterator cell = mg_dof_handler
                                                          .begin_active(),
                                                 endc = mg_dof_handler.end();
  for (; cell != endc; ++cell)
    {
      cell_matrix = 0;
      cell_rhs    = 0;

      fe_values.reinit(cell);

      coefficient.value_list(fe_values.get_quadrature_points(),
                             coefficient_values);

      for (unsigned int q_point = 0; q_point < n_q_points; ++q_point)
        for (unsigned int i = 0; i < dofs_per_cell; ++i)
          {
            for (unsigned int j = 0; j < dofs_per_cell; ++j)
              cell_matrix(i, j) +=
                (coefficient_values[q_point] *
                 fe_values.shape_grad(i, q_point) *
                 fe_values.shape_grad(j, q_point) * fe_values.JxW(q_point));

            cell_rhs(i) += (fe_values.shape_value(i, q_point) * 10.0 *
                            fe_values.JxW(q_point));
          }

      cell->get_dof_indices(local_dof_indices);
      constraints.distribute_local_to_global(
        cell_matrix, cell_rhs, local_dof_indices, system_matrix, system_rhs);
    }
}


template <int dim>
void
LaplaceProblem<dim>::assemble_multigrid()
{
  QGauss<dim> quadrature_formula(1 + degree);

  FEValues<dim> fe_values(fe,
                          quadrature_formula,
                          update_values | update_gradients |
                            update_quadrature_points | update_JxW_values);

  const unsigned int dofs_per_cell = fe.dofs_per_cell;
  const unsigned int n_q_points    = quadrature_formula.size();

  FullMatrix<double> cell_matrix(dofs_per_cell, dofs_per_cell);

  std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

  const Coefficient<dim> coefficient;
  std::vector<double>    coefficient_values(n_q_points);

  std::vector<AffineConstraints<double>> boundary_constraints(
    triangulation.n_levels());
  AffineConstraints<double> empty_constraints;
  for (unsigned int level = 0; level < triangulation.n_levels(); ++level)
    {
      boundary_constraints[level].add_lines(
        mg_constrained_dofs.get_refinement_edge_indices(level));
      boundary_constraints[level].add_lines(
        mg_constrained_dofs.get_boundary_indices(level));
      boundary_constraints[level].close();
    }

  typename DoFHandler<dim>::cell_iterator cell = mg_dof_handler.begin(),
                                          endc = mg_dof_handler.end();

  for (; cell != endc; ++cell)
    {
      cell_matrix = 0;
      fe_values.reinit(cell);

      coefficient.value_list(fe_values.get_quadrature_points(),
                             coefficient_values);

      for (unsigned int q_point = 0; q_point < n_q_points; ++q_point)
        for (unsigned int i = 0; i < dofs_per_cell; ++i)
          for (unsigned int j = 0; j < dofs_per_cell; ++j)
            cell_matrix(i, j) +=
              (coefficient_values[q_point] * fe_values.shape_grad(i, q_point) *
               fe_values.shape_grad(j, q_point) * fe_values.JxW(q_point));

      cell->get_mg_dof_indices(local_dof_indices);

      boundary_constraints[cell->level()].distribute_local_to_global(
        cell_matrix, local_dof_indices, mg_matrices[cell->level()]);

      const unsigned int lvl = cell->level();

      for (unsigned int i = 0; i < dofs_per_cell; ++i)
        for (unsigned int j = 0; j < dofs_per_cell; ++j)
          if (mg_constrained_dofs.at_refinement_edge(lvl,
                                                     local_dof_indices[i]) &&
              !mg_constrained_dofs.at_refinement_edge(lvl,
                                                      local_dof_indices[j]) &&
              ((!mg_constrained_dofs.is_boundary_index(lvl,
                                                       local_dof_indices[i]) &&
                !mg_constrained_dofs.is_boundary_index(
                  lvl,
                  local_dof_indices[j])) // ( !boundary(i) && !boundary(j) )
               ||
               (mg_constrained_dofs.is_boundary_index(lvl,
                                                      local_dof_indices[i]) &&
                local_dof_indices[i] ==
                  local_dof_indices[j]) // ( boundary(i) && boundary(j) &&
                                        // i==j )
               ))
            {
            }
          else
            {
              cell_matrix(i, j) = 0;
            }


      empty_constraints.distribute_local_to_global(
        cell_matrix, local_dof_indices, mg_interface_matrices[cell->level()]);
    }
}



template <int dim>
void
LaplaceProblem<dim>::solve()
{
  typedef SparseMatrix<double> matrix_t;
  typedef Vector<double>       vector_t;

  MGTransferPrebuilt<vector_t> mg_transfer(mg_constrained_dofs);
  mg_transfer.build(mg_dof_handler);

  matrix_t &coarse_matrix = mg_matrices[0];

  SolverControl        coarse_solver_control(1000, 1e-10, false, false);
  SolverCG<vector_t>   coarse_solver(coarse_solver_control);
  PreconditionIdentity id;
  MGCoarseGridIterativeSolver<vector_t,
                              SolverCG<vector_t>,
                              matrix_t,
                              PreconditionIdentity>
    coarse_grid_solver(coarse_solver, coarse_matrix, id);

  typedef PreconditionJacobi<matrix_t>                 Smoother;
  MGSmootherPrecondition<matrix_t, Smoother, vector_t> mg_smoother;
  mg_smoother.initialize(mg_matrices, Smoother::AdditionalData(0.5));
  mg_smoother.set_steps(2);

  mg::Matrix<vector_t> mg_matrix(mg_matrices);
  mg::Matrix<vector_t> mg_interface_up(mg_interface_matrices);
  mg::Matrix<vector_t> mg_interface_down(mg_interface_matrices);

  Multigrid<vector_t> mg(
    mg_matrix, coarse_grid_solver, mg_transfer, mg_smoother, mg_smoother);
  mg.set_edge_matrices(mg_interface_down, mg_interface_up);

  PreconditionMG<dim, vector_t, MGTransferPrebuilt<vector_t>> preconditioner(
    mg_dof_handler, mg, mg_transfer);

  SolverControl      solver_control(500, 1e-8 * system_rhs.l2_norm(), false);
  SolverCG<vector_t> solver(solver_control);

  solution = 0;
  solver.solve(system_matrix, solution, system_rhs, preconditioner);
  constraints.distribute(solution);
}



template <int dim>
void
LaplaceProblem<dim>::refine_grid()
{
  Vector<float> estimated_error_per_cell(triangulation.n_active_cells());

  KellyErrorEstimator<dim>::estimate(
    mg_dof_handler,
    QGauss<dim - 1>(degree + 1),
    std::map<types::boundary_id, const Function<dim> *>(),
    solution,
    estimated_error_per_cell);

  const double threshold = 0.6 * estimated_error_per_cell.linfty_norm();
  GridRefinement::refine(triangulation, estimated_error_per_cell, threshold);

  triangulation.execute_coarsening_and_refinement();
}


template <int dim>
void
LaplaceProblem<dim>::run()
{
  for (unsigned int cycle = 0; cycle < 5; ++cycle)
    {
      deallog << "Cycle " << cycle << ':' << std::endl;

      if (cycle == 0)
        {
          GridGenerator::hyper_cube(triangulation);
          triangulation.refine_global(4);
        }
      else
        refine_grid();


      deallog << "   Number of active cells:       "
              << triangulation.n_active_cells() << std::endl;

      setup_system();

      deallog << "   Number of degrees of freedom: " << mg_dof_handler.n_dofs()
              << " (by level: ";
      for (unsigned int level = 0; level < triangulation.n_levels(); ++level)
        deallog << mg_dof_handler.n_dofs(level)
                << (level == triangulation.n_levels() - 1 ? ")" : ", ");
      deallog << std::endl;

      assemble_system();
      assemble_multigrid();

      solve();
    }
}


int
main()
{
  initlog();
  deallog << std::setprecision(10);

  try
    {
      LaplaceProblem<2> laplace_problem(1);
      laplace_problem.run();
    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl
                << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Exception on processing: " << std::endl
                << exc.what() << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;

      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl
                << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Unknown exception!" << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }

  return 0;
}
