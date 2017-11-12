// Copyright (C) 2014 by Luca Heltai (1), Saswati Roy (2), and
// Francesco Costanzo (3)
//
// (1) Scuola Internazionale Superiore di Studi Avanzati
//     E-mail: luca.heltai@sissa.it
// (2) Center for Neural Engineering, The Pennsylvania State University
//     E-Mail: sur164@psu.edu
// (3) Center for Neural Engineering, The Pennsylvania State University
//     E-Mail: costanzo@engr.psu.edu
//
// This file is subject to LGPL and may not be distributed without
// copyright and license information. Please refer to the webpage
// http://www.dealii.org/ -> License for the text and further
// information on this license.

#include "immersed_fem.h"


// Constructor:
//    Initializes the FEM system of the control volume;
//    Initializes the FEM system of the immersed domain;
//    Initializes, corresponding dof handlers, and the quadrature rule;
//    It runs the <code>create_triangulation_and_dofs</code> function.

template <int dim>
ImmersedFEM<dim>::ImmersedFEM (IFEMParameters<dim> &par)
  :
  par (par),
  fe_f (
    FE_Q<dim>(par.degree),
    dim,
    *FETools::get_fe_by_name<dim,dim>(par.fe_p_name),
    1
  ),
  fe_s (FE_Q<dim, dim>(par.degree), dim),
  dh_f (tria_f),
  dh_s (tria_s),
  quad_f (par.degree+2),
  quad_s (qtrapez, 4*(par.degree+8))
{
  if (par.degree <= 1)
    cout
        << " WARNING: The chosen pair of finite element spaces is not  stable."
        << endl
        << " The obtained results will be nonsense."
        << endl;

  if ( Utilities::match_at_string_start(par.fe_p_name, string("FE_DGP")))
    dgp_for_p = true;
  else dgp_for_p = false;

  create_triangulation_and_dofs ();

  global_info_file.open((par.output_name+"_global.gpl").c_str());
}

// Distructor: deletion of pointers created with <code>new</code> and
// closing of the record keeping file.

template <int dim>
ImmersedFEM<dim>::~ImmersedFEM ()
{
  if (mapping)
    delete mapping;
  global_info_file.close();
}

// Determination of the current value of time dependent boundary
// values.

template <int dim>
void
ImmersedFEM<dim>::compute_current_bc (const double t)
{
  par.u_g.set_time(t);
  VectorTools::interpolate_boundary_values (
    StaticMappingQ1<dim>::mapping,
    dh_f,
    par.boundary_map,
    par.boundary_values,
    par.component_mask
  );


// Set to zero the value of the first dof associated to
// the pressure field.
  if (par.fix_pressure == true) par.boundary_values[constraining_dof] = 0;
}

// Application of time dependent boundary conditions.

template <int dim>
void
ImmersedFEM<dim>::apply_current_bc
(
  BlockVector<double> &vec,
  const double t
)
{
  compute_current_bc(t);
  map<unsigned int, double>::iterator it    = par.boundary_values.begin(),
                                      itend = par.boundary_values.end();

  for (; it != itend; ++it) vec.block(0)(it->first) = it->second;

}



// Defines the triangulations for both the control volume and the
// immersed domain.  It distributes degrees of freedom over said
// triangulations. Both grids are assumed to be available in UCD
// format. The naming convention is as follows:
// <code>fluid_[dim]d.inp</code> for the control volume and
// <code>solid_[dim]d.inp</code> for the immersed domain. This function also
// sets up the framework for enforcing the initial conditions.

template <int dim>
void
ImmersedFEM<dim>::create_triangulation_and_dofs ()
{
  if (par.material_model == IFEMParameters<dim>::CircumferentialFiberModel)
    {
// This is used only by the solution of the problem with the immersed
// domain consisting of a circular cylinder.  We only implemented this
// in two dimensions.
      Assert(dim == 2, ExcNotImplemented());

      ExactSolutionRingWithFibers<dim> ring(par);

// Construct the square domain for the control volume using the parameter file.
      GridGenerator::hyper_cube (tria_f, 0., ring.l);

// Construct the hyper shell using the parameter file.
      GridGenerator::hyper_shell(tria_s, ring.center,
                                 ring.R, ring.R+ring.w);

      static const HyperShellBoundary<dim> shell_boundary(ring.center);
      tria_s.set_boundary(0, shell_boundary);
    }
  else
    {
      // As specified in the documentation for the "GridIn" class the
      // triangulation corresponding to a grid needs to be empty at
      // this time.
      GridIn<dim> grid_in_f;
      grid_in_f.attach_triangulation (tria_f);

      {
        ifstream file (par.fluid_mesh.c_str());
        Assert (file, ExcFileNotOpen (par.fluid_mesh.c_str()));


        // A grid in ucd format is expected.
        grid_in_f.read_ucd (file);
      }

      GridIn<dim, dim> grid_in_s;
      grid_in_s.attach_triangulation (tria_s);

      ifstream file (par.solid_mesh.c_str());
      Assert (file, ExcFileNotOpen (par.solid_mesh.c_str()));

// A grid in ucd format is expected.
      grid_in_s.read_ucd (file);
    }


  cout
      << "Number of fluid refines = "
      << par.ref_f
      << endl;
  tria_f.refine_global (par.ref_f);
  cout
      << "Number of active fluid cells: "
      << tria_f.n_active_cells ()
      << endl;
  cout
      << "Number of solid refines = "
      << par.ref_s
      << endl;
  tria_s.refine_global (par.ref_s);
  cout
      << "Number of active solid cells: "
      << tria_s.n_active_cells ()
      << endl;


// Initialization of the boundary_indicators vector.
  boundary_indicators = tria_f.get_boundary_ids();


// Distribution of the degrees of freedom. Both for the solid
// and fluid domains, the dofs are renumbered first globally
// and then by component.
  dh_f.distribute_dofs (fe_f);
  DoFRenumbering::boost::Cuthill_McKee (dh_f);


// Consistently with the fact that the various components of
// the system are stored in a block matrix, now renumber
// velocity and pressure component wise.
  vector<unsigned int> block_component (dim+1,0);
  block_component[dim] = 1;
  DoFRenumbering::component_wise (dh_f, block_component);

  vector<unsigned int> dofs_per_block (2);
  DoFTools::count_dofs_per_block (dh_f, dofs_per_block, block_component);


// Accounting of the number of degrees of freedom for the fluid
//  domain on a block by block basis.
  n_dofs_u  = dofs_per_block[0];
  n_dofs_p  = dofs_per_block[1];
  n_dofs_up = dh_f.n_dofs ();


// Simply distribute dofs on the solid displacement.
  dh_s.distribute_dofs (fe_s);
  DoFRenumbering::boost::Cuthill_McKee (dh_s);


// Determine the total number of dofs.
  n_dofs_W = dh_s.n_dofs ();
  n_total_dofs = n_dofs_up+n_dofs_W;

  cout
      << "dim (V_h) = "
      << n_dofs_u
      << endl
      << "dim (Q_h) = "
      << n_dofs_p
      << endl
      << "dim (Z_h) = "
      << dh_s.n_dofs ()
      << endl
      << "Total: "
      << n_total_dofs
      << endl;

  vector<unsigned int> all_dofs (2);
  all_dofs[0] = n_dofs_up;
  all_dofs[1] = n_dofs_W;


// Re-initialization of the BlockVectors containing the values of the
// degrees of freedom and of the residual.
  current_xi.reinit (all_dofs);
  previous_xi.reinit (all_dofs);
  current_xit.reinit (all_dofs);
  current_res.reinit (all_dofs);
  newton_update.reinit (all_dofs);

// Re-initialization of the average and unit pressure vectors.
  pressure_average.reinit (n_dofs_up);
  unit_pressure.reinit (n_dofs_up);

// Re-initialization of temporary vectors.
  tmp_vec_n_total_dofs.reinit(n_total_dofs);
  tmp_vec_n_dofs_up.reinit(n_dofs_up);

// Determine the area (in 2D) of the control volume and find the first
// dof pertaining to the pressure.
  get_area_and_first_pressure_dof ();

// The following matrix plays no part in the formulation. It is
// defined here only to use the VectorTools::project function in
// initializing the vectors previous_xi.block(0) and unit_pressure.
  ConstraintMatrix cc;
  cc.close();

// Construction of the initial conditions.
  if (fe_f.has_support_points())
    {
      VectorTools::interpolate (dh_f, par.u_0, previous_xi.block(0));
      VectorTools::interpolate (
        dh_f,
        ComponentSelectFunction<dim>(dim, 1., dim+1),
        unit_pressure
      );
    }
  else
    {
      VectorTools::project (dh_f, cc, quad_f, par.u_0, previous_xi.block(0));
      VectorTools::project (
        dh_f,
        cc,
        quad_f,
        ComponentSelectFunction<dim>(dim, 1., dim+1),
        unit_pressure
      );
    }

  if (fe_s.has_support_points())
    VectorTools::interpolate (dh_s, par.W_0, previous_xi.block(1));
  else
    VectorTools::project (dh_s, cc, quad_s, par.W_0, previous_xi.block(1));

  mapping = new MappingQEulerian<dim, Vector<double>, dim> (par.degree,
                                                            dh_s, previous_xi.block(1));


// We now deal with the sparsity patterns.
  {

    BlockDynamicSparsityPattern csp (2,2);

    csp.block(0,0).reinit (n_dofs_up, n_dofs_up);
    csp.block(0,1).reinit (n_dofs_up, n_dofs_W );
    csp.block(1,0).reinit (n_dofs_W , n_dofs_up);
    csp.block(1,1).reinit (n_dofs_W , n_dofs_W );

// As stated in the documentation, now we <i>must</i> call the function
// <code>csp.collect_sizes.()</code> since have changed the size
// of the sub-objects of the object <code>csp</code>.
    csp.collect_sizes();

    Table< 2, DoFTools::Coupling > coupling(dim+1,dim+1);
    for (unsigned int i=0; i<dim; ++i)
      {

// Velocity is coupled with pressure.
        coupling(i,dim) = DoFTools::always;

// Pressure is coupled with velocity.
        coupling(dim,i) = DoFTools::always;
        for (unsigned int j=0; j<dim; ++j)

// The velocity components are coupled with themselves and each other.
          coupling(i,j) = DoFTools::always;
      }

// The pressure is coupled with itself.
    coupling(dim, dim) = DoFTools::always;


// Find the first pressure dof.  Then tell all the pressure dofs that
// they are related to the first pressure dof.
    set<unsigned int>::iterator it = pressure_dofs.begin();
    constraining_dof = *it;
    for (++it; it != pressure_dofs.end(); ++it)
      {
        csp.block(0,0).add(constraining_dof, *it);
      }

    DoFTools::make_sparsity_pattern (dh_f,
                                     coupling,
                                     csp.block(0,0));

    DoFTools::make_sparsity_pattern (dh_s, csp.block(1,1));

    sparsity.copy_from (csp);
    assemble_sparsity(*mapping);
  }

// Here is the Jacobian matrix.
  JF.reinit(sparsity);


// Boundary conditions at t = 0.
  apply_current_bc(previous_xi, 0);


// Resizing other containers concerning the elastic response of the
// immersed domain.
  A_gamma.reinit(n_dofs_W);
  M_gamma3_inv_A_gamma.reinit(n_dofs_W);


// Creating the mass matrix for the solid domain and storing its
// inverse.
  ConstantFunction<dim> phi_b_func (par.Phi_B, dim);
  M_gamma3.reinit (sparsity.block(1,1));


// Using the <code>deal.II</code> in-built functionality to
// create the mass matrix.
  MatrixCreator::create_mass_matrix (dh_s, quad_s, M_gamma3, &phi_b_func);
  M_gamma3_inv.initialize (M_gamma3);

}


// Relatively standard way to determine the sparsity pattern of each
// block of the global Jacobian.

template <int dim>
void
ImmersedFEM<dim>::assemble_sparsity (Mapping<dim, dim> &immersed_mapping)
{
  FEFieldFunction<dim, DoFHandler<dim>, Vector<double> > up_field (dh_f, tmp_vec_n_dofs_up);

  vector< typename DoFHandler<dim>::active_cell_iterator > cells;
  vector< vector< Point< dim > > > qpoints;
  vector< vector< unsigned int> > maps;
  vector< unsigned int > dofs_f(fe_f.dofs_per_cell);
  vector< unsigned int > dofs_s(fe_s.dofs_per_cell);

  typename DoFHandler<dim,dim>::active_cell_iterator
  cell = dh_s.begin_active(),
  endc = dh_s.end();

  FEValues<dim,dim> fe_v(immersed_mapping, fe_s, quad_s,
                         update_quadrature_points);

  DynamicSparsityPattern sp1(n_dofs_up, n_dofs_W);
  DynamicSparsityPattern sp2(n_dofs_W , n_dofs_up);

  for (; cell != endc; ++cell)
    {
      fe_v.reinit(cell);
      cell->get_dof_indices(dofs_s);
      up_field.compute_point_locations (fe_v.get_quadrature_points(),
                                        cells, qpoints, maps);
      for (unsigned int c=0; c<cells.size(); ++c)
        {
          cells[c]->get_dof_indices(dofs_f);
          for (unsigned int i=0; i<dofs_f.size(); ++i)
            for (unsigned int j=0; j<dofs_s.size(); ++j)
              {
                sp1.add(dofs_f[i],dofs_s[j]);
                sp2.add(dofs_s[j],dofs_f[i]);
              }
        }
    }

  sparsity.block(0,1).copy_from(sp1);
  sparsity.block(1,0).copy_from(sp2);
}

// Determination of the volume (area in 2D) of the control volume and
// identification of the first dof associated with the pressure field.

template <int dim>
void
ImmersedFEM<dim>::get_area_and_first_pressure_dof ()
{
  area = 0.0;
  typename DoFHandler<dim,dim>::active_cell_iterator
  cell = dh_f.begin_active (),
  endc = dh_f.end ();

  FEValues<dim,dim> fe_v (fe_f,
                          quad_f,
                          update_values |
                          update_JxW_values);

  vector<unsigned int> dofs_f(fe_f.dofs_per_cell);


// Calculate the area of the control volume.
  for (; cell != endc; ++cell)
    {
      fe_v.reinit (cell);
      cell->get_dof_indices (dofs_f);

      for (unsigned int i=0; i < fe_f.dofs_per_cell; ++i)
        {
          unsigned int comp_i = fe_f.system_to_component_index(i).first;
          if (comp_i == dim)
            {
              pressure_dofs.insert(dofs_f[i]);
              if (dgp_for_p) break;
            }
        }

      for (unsigned int q=0; q<quad_f.size(); ++q) area += fe_v.JxW(q);

    }


// Get the first dof pertaining to pressure.
  constraining_dof = *(pressure_dofs.begin());

}


// Assemblage of the various operators in the formulation along with
// their contribution to the system Jacobian.

template <int dim>
void
ImmersedFEM<dim>::residual_and_or_Jacobian
(
  BlockVector<double> &residual,
  BlockSparseMatrix<double> &jacobian,
  const BlockVector<double> &xit,
  const BlockVector<double> &xi,
  const double alpha,
  const double t
)
{

// Determine whether or not the calculation of the Jacobian is needed.
  bool update_jacobian = !jacobian.empty();


// Reset the mapping to NULL.
  if (mapping != NULL) delete mapping;


// In a semi-implicit scheme, the position of the immersed body
// coincides with the position of the body at the previous time step.
  if (par.semi_implicit == true)
    {
      mapping = new MappingQEulerian<dim, Vector<double>, dim> (par.degree,
                                                                dh_s,
                                                                previous_xi.block(1));
    }
  else
    mapping = new MappingQEulerian<dim, Vector<double>, dim> (par.degree,
                                                              dh_s,
                                                              xi.block(1));


// In applying the boundary conditions, we set a scaling factor equal
// to the diameter of the smallest cell in the triangulation.
  scaling = GridTools::minimal_cell_diameter(tria_f);

// Initialization of the residual.
  residual = 0;

// If the Jacobian is needed, then it is initialized here.
  if (update_jacobian)
    {
      jacobian.clear();
      assemble_sparsity(*mapping);
      jacobian.reinit(sparsity);
    }


// Evaluation of the current values of the external force and of the
// boundary conditions.
  par.force.set_time(t);
  compute_current_bc(t);


// Computation of the maximum number of degrees of freedom one could
// have on a fluid-solid interaction cell.  <b>Rationale</b> the coupling
// of the fluid and solid domains is computed by finding each of the
// fluid cells that interact with a given solid cell. In each
// interaction instance we will be dealing with a total number of
// degrees of freedom that is the sum of the dofs of the current
// solid cell and the dofs of the current fluid cell in the list of
// fluid cells interacting with the solid cell in question.
  unsigned int n_local_dofs = fe_f.dofs_per_cell + fe_s.dofs_per_cell;


// Storage for the local dofs in the fluid and in the solid.
  vector< unsigned int > dofs_f(fe_f.dofs_per_cell);
  vector< unsigned int > dofs_s(fe_s.dofs_per_cell);


// <code>FEValues</code> for the fluid.
  FEValues<dim> fe_f_v (fe_f,
                        quad_f,
                        update_values |
                        update_gradients |
                        update_JxW_values |
                        update_quadrature_points);


// Number of quadrature points on fluid and solid cells.
  const unsigned int nqpf = quad_f.size();
  const unsigned int nqps = quad_s.size();


// The local residual vector: the largest possible size of this
// vector is <code>n_local_dofs</code>.
  vector<double> local_res(n_local_dofs);
  vector<Vector<double> > local_force(nqpf, Vector<double>(dim+1));
  FullMatrix<double> local_jacobian;
  if (update_jacobian) local_jacobian.reinit(n_local_dofs, n_local_dofs);


// Since we want to solve a system of equations of the form
// \f$f(\xi', \xi, t) = 0\f$,
// we need to manage the information in \f$\xi'\f$ as though it were
// independent of the information in \f$\xi\f$. We do so by defining a
// vector of local degrees of freedom that has a length equal
// to twice the total number of local degrees of freedom.
// This information is stored in the vector <code>local_x</code>.
// <ul>
// <li> The first <code>fe_f.dofs_per_cell</code> elements of
//      <code>local_x</code> contain the elements of \f$\xi'\f$
//      corresponding to the current fluid cell.
// <li> The subsequent <code>fe_s.dofs_per_cell</code> elements of
//      <code>local_x</code> contain the elements of \f$\xi'\f$ corresponding to the
//      current solid cell.
// <li> The subsequent <code>fe_f.dofs_per_cell</code> elements of
//      <code>local_x</code> contain the elements of \f$\xi\f$ corresponding to the
//      current fluid cell.
// <li> The subsequent <code>fe_s.dofs_per_cell</code> elements of
//      <code>local_x</code>.
// <ul>

// Definition of the local dependent variables for the fluid.
  vector<Vector<double> > local_upt(nqpf, Vector<double>(dim+1));
  vector<Vector<double> > local_up (nqpf, Vector<double>(dim+1));
  vector< vector< Tensor<1,dim> > > local_grad_up(
    nqpf,
    vector< Tensor<1,dim> >(dim+1)
  );
  unsigned int comp_i = 0, comp_j = 0;


// Initialization of the local contribution to the pressure
// average.
  double local_average_pressure = 0.0;
  vector<double> local_pressure_coefficient(n_local_dofs);


// ------------------------------------------------------------
// OPERATORS DEFINED OVER THE ENTIRE DOMAIN: BEGIN
// ------------------------------------------------------------

// We now determine the contribution to the residual due to the
// fluid.  This is the standard Navier-Stokes component of the
// problem.  As such, the contributions are to the equation in
// \f$V'\f$ and to the equation in \f$Q'\f$.


// These iterators point to the first and last active cell of
// the fluid domain.
  typename DoFHandler<dim>::active_cell_iterator
  cell = dh_f.begin_active(),
  endc = dh_f.end();


// Cycle over the cells of the fluid domain.
  for (; cell != endc; ++cell)
    {
      cell->get_dof_indices(dofs_f);


// Re-initialization of the <code>FEValues</code>.
      fe_f_v.reinit(cell);


// Values of the partial derivative of the velocity relative to time
// at the quadrature points on the current fluid cell.  Strictly
// speaking, this vector also includes values of the partial
// derivative of the pressure with respect to time.
      fe_f_v.get_function_values(xit.block(0), local_upt);


// Values of the velocity at the quadrature points on the current
// fluid cell. Strictly speaking, this vector also includes values of
// pressure.
      fe_f_v.get_function_values(xi.block(0), local_up);


// Values of the gradient of the velocity at the quadrature points of
// the current fluid cell.
      fe_f_v.get_function_gradients(xi.block(0), local_grad_up);


// Values of the body force at the quadrature points of the current
// fluid cell.
      par.force.vector_value_list(fe_f_v.get_quadrature_points(), local_force);


// Initialization of the local residual and local Jacobian.
      set_to_zero(local_res);
      if (update_jacobian) set_to_zero(local_jacobian);


// Initialization of the local pressure contribution.
      local_average_pressure = 0.0;
      set_to_zero(local_pressure_coefficient);

      for (unsigned int i=0; i<fe_f.dofs_per_cell; ++i)
        {
          comp_i = fe_f.system_to_component_index(i).first;
          for (unsigned int q=0; q< nqpf; ++q)

// -------------------------------------
// Contribution to the equation in \f$V'\f$.
// -------------------------------------
            if (comp_i < dim)
              {

// \f$\rho [(\partial u/\partial t) - b ) \cdot v - p (\nabla \cdot v)\f$
                local_res[i] += par.rho
                                * ( local_upt[q](comp_i)
                                    -   local_force[q](comp_i) )
                                * fe_f_v.shape_value(i,q)
                                * fe_f_v.JxW(q)
                                - local_up[q](dim)
                                * fe_f_v.shape_grad(i,q)[comp_i]
                                * fe_f_v.JxW(q);
                if (update_jacobian)
                  {
                    for (unsigned int j=0; j<fe_f.dofs_per_cell; ++j)
                      {
                        comp_j = fe_f.system_to_component_index(j).first;
                        if ( comp_i == comp_j )
                          local_jacobian(i,j) += par.rho
                                                 * alpha
                                                 * fe_f_v.shape_value(i,q)
                                                 * fe_f_v.shape_value(j,q)
                                                 * fe_f_v.JxW(q);
                        if ( comp_j == dim )
                          local_jacobian(i,j) -= fe_f_v.shape_grad(i,q)[comp_i]
                                                 * fe_f_v.shape_value(j,q)
                                                 * fe_f_v.JxW(q);
                      }
                  }

// \f$\eta [\nabla_{x} u + (\nabla_{x} u)^{T}] \cdot \nabla v + \rho (\nabla_{x} u) \cdot v\f$.
                for (unsigned int d=0; d<dim; ++d)
                  {
                    local_res[i] += par.eta
                                    * ( local_grad_up[q][comp_i][d]
                                        +
                                        local_grad_up[q][d][comp_i] )
                                    * fe_f_v.shape_grad(i,q)[d]
                                    * fe_f_v.JxW(q)
                                    + par.rho
                                    * local_grad_up[q][comp_i][d]
                                    * local_up[q](d)
                                    * fe_f_v.shape_value(i,q)
                                    * fe_f_v.JxW(q);
                  }
                if ( update_jacobian )
                  {
                    for (unsigned int j=0; j<fe_f.dofs_per_cell; ++j)
                      {
                        comp_j = fe_f.system_to_component_index(j).first;
                        if ( comp_j == comp_i )
                          for ( unsigned int d = 0; d < dim; ++d )
                            local_jacobian(i,j)  += par.eta
                                                    * fe_f_v.shape_grad(i,q)[d]
                                                    * fe_f_v.shape_grad(j,q)[d]
                                                    * fe_f_v.JxW(q)
                                                    + par.rho
                                                    * fe_f_v.shape_value(i,q)
                                                    * local_up[q](d)
                                                    * fe_f_v.shape_grad(j,q)[d]
                                                    * fe_f_v.JxW(q);
                        if (comp_j < dim)
                          local_jacobian(i,j)   += par.eta
                                                   * fe_f_v.shape_grad(i,q)[comp_j]
                                                   * fe_f_v.shape_grad(j,q)[comp_i]
                                                   * fe_f_v.JxW(q)
                                                   + par.rho
                                                   * local_grad_up[q][comp_i][comp_j]
                                                   * fe_f_v.shape_value(i,q)
                                                   * fe_f_v.shape_value(j,q)
                                                   * fe_f_v.JxW(q);
                      }
                  }
              }
            else
              {

// ------------------------------------
// Contribution to the equation in Q'.
// ------------------------------------

// \f$-q (\nabla_{x} \cdot u)\f$
                for (unsigned int d=0; d<dim; ++d)
                  local_res[i] -= local_grad_up[q][d][d]
                                  * fe_f_v.shape_value(i,q)
                                  * fe_f_v.JxW(q);
                if ( update_jacobian )
                  for (unsigned int j=0; j<fe_f.dofs_per_cell; ++j)
                    {
                      comp_j = fe_f.system_to_component_index(j).first;
                      if ( comp_j < dim )
                        local_jacobian(i,j) -= fe_f_v.shape_value(i,q)
                                               * fe_f_v.shape_grad(j,q)[comp_j]
                                               * fe_f_v.JxW(q);
                    }

                if (par.all_DBC && !par.fix_pressure)
                  {
                    if (
                      !dgp_for_p
                      ||
                      (dgp_for_p && (fe_f.system_to_component_index(i).second==0))
                    )
                      {
                        local_average_pressure += xi.block(0)(dofs_f[i])
                                                  *fe_f_v.shape_value(i,q)
                                                  *fe_f_v.JxW(q);
                        if (update_jacobian)
                          {
                            local_pressure_coefficient[i] += fe_f_v.shape_value(i,q)
                                                             *fe_f_v.JxW(q);
                          }
                      }
                  }
              }
        }

// Apply boundary conditions.
      apply_constraints (local_res,
                         local_jacobian,
                         xi.block(0),
                         dofs_f);


// Now the contribution to the residual due to the current cell
// is assembled into the global system's residual.
      distribute_residual(residual.block(0), local_res, dofs_f, 0);
      if (update_jacobian)
        distribute_jacobian (JF.block(0,0),
                             local_jacobian,
                             dofs_f,
                             dofs_f,
                             0,
                             0);

      if (par.all_DBC && !par.fix_pressure)
        {
          distribute_constraint_on_pressure (residual.block(0),
                                             local_average_pressure);

          if (update_jacobian)
            distribute_constraint_on_pressure (jacobian.block(0,0),
                                               local_pressure_coefficient,
                                               dofs_f,
                                               0);
        }
    }


// -----------------------------------------
// OPERATORS DEFINED OVER ENTIRE DOMAIN: END
// -----------------------------------------


// -------------------------------------------------
// OPERATORS DEFINED OVER THE IMMERSED DOMAIN: BEGIN
// -------------------------------------------------

// We distinguish two orders of organization:
//  <ol>
// <li> we have a cycle
// over the cells of the immersed domain.  For each cell of the
// immersed domain we determine the cells in the fluid domain
// interacting with the cell in question.  Then we cycle over each of
// the fluid cell.
//
// <li> The operators defined over the immersed
// domain contribute to all three of the equations forming the
// problem.  We group the operators in question by equation.
// Specifically, we first deal with the terms that contribute to the
// equation in \f$V'\f$, then we deal with the terms that contribute to \f$Q'\f$,
// and finally we deal with the terms that contribute to \f$Y'\f$.
// </ol>
// <b>Note:</b> In the equation in \f$Y'\f$ there is contribution that does
// not arise from the interaction of solid and fluid.



// Representation of the velocity and pressure in the control volume
// as a field.
  FEFieldFunction<dim, DoFHandler<dim>, Vector<double> >
  up_field (dh_f, xi.block(0));


// Containers to store the information on the interaction of the
// current solid cell with the corresponding set of fluid cells that
// happen to contain the quadrature points of the solid cell in
// question.
  vector< typename DoFHandler<dim>::active_cell_iterator > fluid_cells;
  vector< vector< Point< dim > > > fluid_qpoints;
  vector< vector< unsigned int> > fluid_maps;


// Local storage of the
// <ul>
//  <li> velocity in the solid (\f$\partial w/\partial t\f$): <code>local_Wt</code>;
//  <li> displacement in the solid (\f$w\f$): <code>local_W</code>;
//  <li> first Piola-Kirchhoff stress: <code>Pe</code>;
//  <li> deformation gradient (\f$F\f$): <code>F</code>;
//  <li> \f$P_{s}^{e} F^{T}\f$, which is the work conjugate of the velocity
//       gradient when measured over the deformed configuration:
//       <code>PeFT</code>;
//  <li> Frechet derivative of \f$P_{s}^{e} F^{T}\f$ with respect to degrees of
//    freedom in a solid cell: <code>DPeFT_dxi</code>.
// </ul>
  vector<Vector<double> > local_Wt(nqps, Vector<double>(dim));
  vector<Vector<double> > local_W (nqps, Vector<double>(dim));
  vector<Tensor<2,dim,double> > Pe(nqps, Tensor<2,dim,double>());
  vector<Tensor<2,dim,double> > F(nqps, Tensor<2,dim,double>());
  Tensor<2,dim,double> PeFT;
  vector< vector<Tensor<2,dim,double> > > DPeFT_dxi;
  if (update_jacobian)
    {
      DPeFT_dxi.resize(nqps, vector< Tensor<2,dim,double> >
                       (fe_s.dofs_per_cell, Tensor<2,dim,double>()));
    }


// Initialization of the elastic operator of the immersed
// domain.
  A_gamma = 0.0;

// Definition of the local contributions to \f$A_{\gamma}\f$ and the product of
// the inverse of the mass matrix of the immersed domain with \f$A_{\gamma}\f$.
  Vector<double> local_A_gamma (fe_s.dofs_per_cell);
  Vector<double> local_M_gamma3_inv_A_gamma (fe_s.dofs_per_cell);


// This information is used in finding what fluid cell contain the
// solid domain at the current time.
  FEValues<dim,dim> fe_v_s_mapped (*mapping,
                                   fe_s,
                                   quad_s,
                                   update_quadrature_points);


// <code>FEValues</code> to carry out integrations over the solid domain.
  FEValues<dim,dim> fe_v_s(fe_s,
                           quad_s,
                           update_quadrature_points |
                           update_values |
                           update_gradients |
                           update_JxW_values);


// Iterators pointing to the beginning and end cells
// of the active triangulation for the solid domain.
  typename DoFHandler<dim,dim>::active_cell_iterator
  cell_s = dh_s.begin_active(),
  endc_s = dh_s.end();


// Now we cycle over the cells of the solid domain to evaluate \f$A_{\gamma}\f$
// and \f$M_{\gamma 3}^{-1} A_{\gamma}\f$.
  for (; cell_s != endc_s; ++cell_s)
    {
      fe_v_s.reinit (cell_s);
      cell_s->get_dof_indices (dofs_s);
      get_Agamma_values (fe_v_s, dofs_s, xi.block(1), local_A_gamma);
      A_gamma.add (dofs_s, local_A_gamma);
    }

  M_gamma3_inv_A_gamma = A_gamma;
  M_gamma3_inv.solve (M_gamma3_inv_A_gamma);


// -----------------------------------------------
// Cycle over the cells of the solid domain: BEGIN
// -----------------------------------------------
  for (cell_s = dh_s.begin_active(); cell_s != endc_s; ++cell_s)
    {
      fe_v_s_mapped.reinit(cell_s);
      fe_v_s.reinit(cell_s);
      cell_s->get_dof_indices(dofs_s);


// Localization of the current independent variables for the immersed
// domain.
      fe_v_s.get_function_values (xit.block(1), local_Wt);
      fe_v_s.get_function_values ( xi.block(1), local_W);
      localize (local_M_gamma3_inv_A_gamma, M_gamma3_inv_A_gamma, dofs_s);
      get_Pe_F_and_DPeFT_dxi_values (fe_v_s,
                                     dofs_s,
                                     xi.block(1),
                                     update_jacobian,
                                     Pe,
                                     F,
                                     DPeFT_dxi);


// Coupling between fluid and solid.  Identification of the fluid
// cells containing the quadrature points on the current solid cell.
      up_field.compute_point_locations (fe_v_s_mapped.get_quadrature_points(),
                                        fluid_cells,
                                        fluid_qpoints,
                                        fluid_maps);

      local_force.resize (nqps, Vector<double>(dim+1));
      par.force.vector_value_list (fe_v_s_mapped.get_quadrature_points(),
                                   local_force);


// Cycle over all of the fluid cells that happen to contain some of
// the the quadrature points of the current solid cell.
      for (unsigned int c=0; c<fluid_cells.size(); ++c)
        {
          fluid_cells[c]->get_dof_indices (dofs_f);


// Local <code>FEValues</code> of the fluid
          Quadrature<dim> local_quad (fluid_qpoints[c]);
          FEValues<dim> local_fe_f_v (fe_f,
                                      local_quad,
                                      update_values |
                                      update_gradients |
                                      update_hessians);
          local_fe_f_v.reinit(fluid_cells[c]);


// Construction of the values at the quadrature points of the current
// solid cell of the velocity of the fluid.
          local_up.resize (local_quad.size(), Vector<double>(dim+1));
          local_fe_f_v.get_function_values (xi.block(0), local_up);

// A bit of nomenclature:
// <dl>
// <dt>Equation in \f$V'\f$</dt>
//     <dd> Assemblage of the terms in the equation in \f$V'\f$ that
//          are defined over \f$B\f$.</dd>

// <dt>Equation in \f$Y'\f$</dt>
//     <dd> Assemblage of the terms in the equation in \f$Y'\f$ that involve
//          the velocity \f$u\f$. </dd>
// </dl>




// Equation in \f$V'\f$: initialization of residual.
          set_to_zero(local_res);
          if (update_jacobian) set_to_zero(local_jacobian);

// Equation in \f$V'\f$: begin cycle over fluid dofs
          for (unsigned int i=0; i<fe_f.dofs_per_cell; ++i)
            {
              comp_i = fe_f.system_to_component_index(i).first;
              if (comp_i < dim)
                for (unsigned int q=0; q<local_quad.size(); ++q)
                  {

// Quadrature point on the <i>mapped</i> solid (\f$B_{t}\f$).
                    unsigned int &qs = fluid_maps[c][q];


// Contribution due to the elastic component of the stress response
// function in the solid:  \f$P_{s}^{e} F^{T} \cdot \nabla_{x} v\f$.
                    if ((!par.semi_implicit) || (!par.use_spread))
                      PeFT = contract<1,1>(Pe[qs],F[qs]);
                    if (!par.use_spread)
                      {
                        local_res[i] += (PeFT[comp_i]
                                         * local_fe_f_v.shape_grad(i,q))
                                        * fe_v_s.JxW(qs);
                        if (update_jacobian)
// Recall that the Hessian is symmetric.
                          {
                            for ( unsigned int j = 0; j < fe_s.dofs_per_cell; ++j )
                              {
                                unsigned int wj = j + fe_f.dofs_per_cell;
                                unsigned int comp_j = fe_s.system_to_component_index(j).first;

                                local_jacobian(i,wj) += ( DPeFT_dxi[qs][j][comp_i]
                                                          * local_fe_f_v.shape_grad(i,q) )
                                                        * fe_v_s.JxW(qs);
                                if ( !par.semi_implicit )
                                  local_jacobian(i,wj) += ( PeFT[comp_i]
                                                            * local_fe_f_v.shape_hessian(i,q)[comp_j])
                                                          * fe_v_s.shape_value(j,qs)
                                                          * fe_v_s.JxW(qs);
                              }
                          }
                      }
                    else
                      {
                        for ( unsigned int j = 0; j < fe_s.dofs_per_cell; ++j )
// The spread operator
                          {
                            unsigned int comp_j = fe_s.system_to_component_index(j).first;
                            if (comp_i == comp_j)
                              local_res[i] += par.Phi_B
                                              * local_fe_f_v.shape_value(i,q)
                                              * fe_v_s.shape_value(j, qs)
                                              * local_M_gamma3_inv_A_gamma(j)
                                              * fe_v_s.JxW(qs);

                            if (update_jacobian)
                              {
                                unsigned int wj = j + fe_f.dofs_per_cell;

                                local_jacobian(i,wj) += ( DPeFT_dxi[qs][j][comp_i]
                                                          * local_fe_f_v.shape_grad(i,q) )
                                                        * fe_v_s.JxW(qs);
                                if ( !par.semi_implicit )
                                  local_jacobian(i,wj) += ( PeFT[comp_i]
                                                            *
                                                            local_fe_f_v.shape_hessian(i,q)[comp_j])
                                                          * fe_v_s.shape_value(j,qs)
                                                          * fe_v_s.JxW(qs);
                              }
                          }
                      }
                  }
            }


// Equation in \f$V'\f$ add to global residual
          apply_constraints(local_res,
                            local_jacobian,
                            xi.block(0),
                            dofs_f);
          distribute_residual(residual.block(0),
                              local_res,
                              dofs_f,
                              0);
          if ( update_jacobian ) distribute_jacobian(JF.block(0,1),
                                                       local_jacobian,
                                                       dofs_f,
                                                       dofs_s,
                                                       0,
                                                       fe_f.dofs_per_cell);


// ****************************************************
// Equation in \f$V'\f$: COMPLETED
// Equation in \f$Y'\f$: NOT YET COMPLETED
// ****************************************************


// Equation in \f$Y'\f$: initialization of residual.
          set_to_zero(local_res);
          if (update_jacobian) set_to_zero(local_jacobian);


// Equation in \f$Y'\f$: begin cycle over dofs of immersed domain.
          for (unsigned int i=0; i<fe_s.dofs_per_cell; ++i)
            {
              unsigned int wi = i + fe_f.dofs_per_cell;
              comp_i = fe_s.system_to_component_index(i).first;
              for (unsigned int q=0; q<local_quad.size(); ++q)
                {
                  unsigned int &qs = fluid_maps[c][q];

// \f$- u(x,t)\big|_{x = s + w(s,t)} \cdot y(s)\f$.
                  local_res[wi] -= par.Phi_B
                                   * local_up[q](comp_i)
                                   * fe_v_s.shape_value(i,qs)
                                   * fe_v_s.JxW(qs);
                  if ( update_jacobian )
                    for (unsigned int j = 0; j < fe_f.dofs_per_cell; ++j)
                      {
                        comp_j = fe_f.system_to_component_index(j).first;
                        if ( comp_i == comp_j )
                          {
                            local_jacobian(wi,j) -= par.Phi_B
                                                    * fe_v_s.shape_value(i,qs)
                                                    * local_fe_f_v.shape_value(j,q)
                                                    * fe_v_s.JxW(qs);
                            if ( !par.semi_implicit )
                              for (unsigned int k = 0; k < fe_s.dofs_per_cell; ++k)
                                {
                                  unsigned int wk = k + fe_f.dofs_per_cell;
                                  unsigned int comp_k = fe_s.system_to_component_index(k).first;
                                  local_jacobian(wi,wk) -= par.Phi_B
                                                           * fe_v_s.shape_value(i,qs)
                                                           * fe_v_s.shape_value(k,qs)
                                                           * local_fe_f_v.shape_grad(j,q)[comp_k]
                                                           * xi.block(0)(dofs_f[j])
                                                           * fe_v_s.JxW(qs);
                                }
                          }
                      }
                }
            }


// Equation in Y': add to global residual.
          apply_constraints(local_res,
                            local_jacobian,
                            xi.block(0),
                            dofs_f);
          distribute_residual(residual.block(1),
                              local_res,
                              dofs_s,
                              fe_f.dofs_per_cell);
          if ( update_jacobian )
            {
              distribute_jacobian (JF.block(1,0),
                                   local_jacobian,
                                   dofs_s,
                                   dofs_f,
                                   fe_f.dofs_per_cell,
                                   0);
              if ( !par.semi_implicit ) distribute_jacobian (JF.block(1,1),
                                                               local_jacobian,
                                                               dofs_s,
                                                               dofs_s,
                                                               fe_f.dofs_per_cell,
                                                               fe_f.dofs_per_cell);
            }



// ***************************
// Equation in \f$V'\f$: COMPLETED
// Equation in \f$Y'\f$: COMPLETED
// ***************************
        }


// Here we assemble the term in the equation
// in \f$Y'\f$ involving \f$\partial w/\partial t\f$: this term does not
// involve any relations concerning the fluid cells.
      set_to_zero(local_res);
      if (update_jacobian) set_to_zero(local_jacobian);

      for (unsigned int i=0; i<fe_s.dofs_per_cell; ++i)
        {
          comp_i = fe_s.system_to_component_index(i).first;
          unsigned int wi = i + fe_f.dofs_per_cell;
          for (unsigned int qs=0; qs<nqps; ++qs)
            {

// \f$(\partial w/\partial t) \cdot y\f$.
              local_res[wi] += par.Phi_B
                               * local_Wt[qs](comp_i)
                               * fe_v_s.shape_value(i,qs)
                               * fe_v_s.JxW(qs);
              if ( update_jacobian )
                for (unsigned int j=0; j<fe_s.dofs_per_cell; ++j)
                  {
                    comp_j = fe_s.system_to_component_index(j).first;
                    unsigned int wj = j + fe_f.dofs_per_cell;
                    if ( comp_i == comp_j )
                      local_jacobian(wi,wj) += par.Phi_B
                                               * alpha
                                               * fe_v_s.shape_value(i,qs)
                                               * fe_v_s.shape_value(j,qs)
                                               * fe_v_s.JxW(qs);
                  }

            }
        }

// We now assemble the contribution just computed into the global
// residual.
      distribute_residual (residual.block(1),
                           local_res,
                           dofs_s,
                           fe_f.dofs_per_cell);
      if ( update_jacobian ) distribute_jacobian (JF.block(1,1),
                                                    local_jacobian,
                                                    dofs_s,
                                                    dofs_s,
                                                    fe_f.dofs_per_cell,
                                                    fe_f.dofs_per_cell);

    }
// Cycle over the cells of the solid domain: END.


// -----------------------------------------------
// OPERATORS DEFINED OVER THE IMMERSED DOMAIN: END
// -----------------------------------------------
}

// Central management of the time stepping scheme.

template <int dim>
void
ImmersedFEM<dim>::run ()
{

// Initialization of the time step counter and of the time variable.
  unsigned int time_step = 1;
  double t = par.dt;


// Initialization of the current state of the system.
  current_xi = previous_xi;


// The variable <code>update_Jacobian</code> is set to true so to have a
// meaningful first update of the solution.
  bool update_Jacobian = true;


// Write the initial conditions in the output file.
  output_step(0.0, previous_xi, 0, par.dt);


// The overall cycle over time begins here.
  for (; t<=par.T; t+= par.dt, ++time_step)
    {

// Initialization of two counters for monitoring the progress of the
// nonlinear solver.
      unsigned int       nonlin_iter = 0;
      unsigned int outer_nonlin_iter = 0;

// Impose the Dirichlet boundary conditions pertaining to the current time
// on the state of the system
      apply_current_bc(current_xi,t);

// The nonlinear solver iteration cycle begins here.
      while (true)
        {

// We view our system of equations to be of the following form:
//
// \f$f(\xi', \xi, t) = 0, \quad \xi(0) = \xi_{0}\f$.
//
// Denoting the current time step by \f$n\f$, the vector \f$\xi'(t_{n})\f$ is
// assumed to be a linear combination of \f$\xi(t_{i})\f$, with \f$i = n - m
// \ldots n\f$, with \f$m \le n\f$. For simplicity, here we implement an implicit
// Euler method, according to which \f$\xi'(t_{n}) = [\xi(t_{n}) -
// \xi(t_{n-1})]/dt\f$, where \f$dt\f$ is the size of the time step.


// Time derivative of the system's state.
          // current_xit.sadd (0, 1./par.dt, current_xi, -1./par.dt, previous_xi); // Deprecated!!!

          current_xit  = current_xi;
          current_xit -= previous_xi;
          current_xit /= par.dt;

          if (update_Jacobian == true)
            {

// Determine the residual and the Jacobian of the residual.
              residual_and_or_Jacobian (current_res,
                                        JF,
                                        current_xit,
                                        current_xi,
                                        1./par.dt,
                                        t);


// Inverse of the Jacobian.
              JF_inv.initialize (JF);


// Reset the <code>update_Jacobian</code> variable to the value specified
// in the parameter file.
              update_Jacobian = par.update_jacobian_continuously;
            }
          else
            {

// Determine the residual but do not update the Jacobian.
              residual_and_or_Jacobian (current_res,
                                        dummy_JF,
                                        current_xit,
                                        current_xi,
                                        0,
                                        t);

            }

// Norm of the residual.
          const double res_norm = current_res.l2_norm();


// Is the norm of the residual sufficiently small?
          if ( res_norm < 1e-10 )
            {

// Make a note and advance to the next step.
              printf (
                " Step %03d, Res:  %-16.3e (converged in %d iterations)\n\n",
                time_step,
                res_norm,
                nonlin_iter
              );
              break;
            }
          else
            {

// If the norm of the residual is not sufficiently small, make a note
// of it and compute an update.
              cout
                  << nonlin_iter
                  << ": "
                  << res_norm
                  << endl;


// To compute the update to the current \f$\xi\f$, we first change the sign
// of the current value of the residual ...
              current_res *= -1;

// ... then we compute the update, which is returned by the method
// <code>solve</code> of the object <code>JF_inv</code>. The latter is of class
// <code>SparseDirectUMFPACK</code> and therefore the value of the (negative) of
// the current residual must be supplied in a container of type
// <code>Vector<double></code>.  So, we first transfer the information in
// <code>current_res</code> into temporary storage, and then we carry out the
// computation of the update.
              tmp_vec_n_total_dofs = current_res;
              JF_inv.solve(tmp_vec_n_total_dofs);

// Now that we have the updated of the solution into an object of type
// <code>Vector<double></code>, we repackage it into an object of
// type <code>BlockVector</code>.
              newton_update = tmp_vec_n_total_dofs;


// Finally, we determine the value of the updated solution.
              current_xi.add(1., newton_update);


// We are here because the solution needed to be updated. The update
// was computed using whatever Jacobian was available.  If, on
// entering this section of the loop, the value of the residual was
// very poor and if the solution's method indicated in the parameter
// file did not call for a continuous update of the Jacobian, now we
// make sure that the Jacobian is updated before computing the next
// solution update.
              if (res_norm > 1e-2) update_Jacobian = true;
            }


// We are here because the solution needed an update. So, start
// counting how many iterations are needed to converge.  If
// convergence is not achieved in 15 iterations update the Jacobian
// and try again.  The maximum number of 15-iteration cycles is set
// (arbitrarily) to three. The counter for the cycle is
// <code>outer_nonlin_iter</code>.
          ++nonlin_iter;
          if (nonlin_iter == 15)
            {
              update_Jacobian = true;
              nonlin_iter = 0;
              outer_nonlin_iter++;
              printf(
                "   %-16.3e (not converged in 15 iterations. Step %d)\n\n",
                res_norm,
                outer_nonlin_iter
              );
            }


// If convergence is not in our destiny, accept defeat, with as much
// grace as it can be mustered, and go home.
          AssertThrow (outer_nonlin_iter <= 3,
                       ExcMessage ("No convergence in nonlinear solver."));
        }


// We have computed a new solution.  So, we update the state of the
// system and move to the next time step.
      previous_xi = current_xi;
      output_step (t, current_xi, time_step, par.dt);
      update_Jacobian = par.update_jacobian_continuously;
      if (par.update_jacobian_at_step_beginning) update_Jacobian = true;

    }
// End of the cycle over time.

  if (par.material_model == IFEMParameters<dim>::CircumferentialFiberModel)
    calculate_error();

}
// End of <code>run()</code>.


// Writes results to the output file.

template <int dim>
void
ImmersedFEM<dim>::output_step
(
  const double t,
  const BlockVector<double> &solution,
  const unsigned int step,
  const double h
)
{
  cout
      << "Time "
      << t
      << ", Step "
      << step
      << ", dt = "
      << h
      << endl;

  global_info_file
      << t
      << " ";

  if ((step ==1) || (step % par.output_interval==0))
    {
      {
        vector<string> joint_solution_names (dim, "v");
        joint_solution_names.push_back ("p");
        DataOut<dim> data_out;
        data_out.attach_dof_handler (dh_f);
        vector< DataComponentInterpretation::DataComponentInterpretation >
        component_interpretation (dim+1,
                                  DataComponentInterpretation::component_is_part_of_vector);
        component_interpretation[dim]
          = DataComponentInterpretation::component_is_scalar;

        data_out.add_data_vector (
          solution.block(0),
          joint_solution_names,
          DataOut<dim>::type_dof_data,
          component_interpretation
        );

        data_out.build_patches (par.degree);
        ofstream output ((par.output_name
                          + "-fluid-"
                          + Utilities::int_to_string (step, 5)
                          + ".vtu").c_str());

        data_out.write_vtu (output);
      }
      {

        vector<string> joint_solution_names (dim, "W");
        DataOut<dim> data_out;
        data_out.attach_dof_handler (dh_s);
        vector< DataComponentInterpretation::DataComponentInterpretation >
        component_interpretation (dim,
                                  DataComponentInterpretation::component_is_part_of_vector);

        data_out.add_data_vector (solution.block(1),
                                  joint_solution_names,
                                  DataOut<dim>::type_dof_data,
                                  component_interpretation);

        data_out.build_patches (*mapping);
        ofstream output ((par.output_name
                          + "-solid-"
                          + Utilities::int_to_string (step, 5)
                          + ".vtu").c_str());
        data_out.write_vtu (output);
      }
    }
  {


// Assemble in and out flux.
    typename DoFHandler<dim,dim>::active_cell_iterator
    cell = dh_f.begin_active(),
    endc = dh_f.end();
    QGauss<dim-1> face_quad(par.degree+2);
    FEFaceValues<dim,dim> fe_v (fe_f,
                                face_quad,
                                update_values |
                                update_JxW_values |
                                update_normal_vectors);

    vector<Vector<double> > local_vp(face_quad.size(),
                                     Vector<double>(dim+1));

    double flux=0;
    for (; cell != endc; ++cell)
      for (unsigned int f=0; f<GeometryInfo<dim>::faces_per_cell; ++f)
        if (cell->face(f)->at_boundary())
          {
            fe_v.reinit(cell, f);
            fe_v.get_function_values(solution.block(0), local_vp);
            const vector<Tensor<1,dim> > &normals = fe_v.get_all_normal_vectors();
            for (unsigned int q=0; q<face_quad.size(); ++q)
              {
                Tensor<1,dim> vq;
                for (unsigned int d=0; d<dim; ++d) vq[d] = local_vp[q](d);
                flux += (vq*normals[q])*fe_v.JxW(q);
              }
          }
    global_info_file
        << flux
        << " ";
  }
  {


// Compute area of the solid, and location of its center of mass.
    typename DoFHandler<dim,dim>::active_cell_iterator
    cell = dh_s.begin_active(),
    endc = dh_s.end();
    FEValues<dim,dim> fe_v(*mapping, fe_s,
                           quad_s,
                           update_JxW_values |
                           update_quadrature_points);

    vector<Vector<double> > local_X(quad_s.size(),
                                    Vector<double>(dim+1));
    double area=0;
    Point<dim> center;
    for (; cell != endc; ++cell)
      {
        fe_v.reinit(cell);
        const vector<Point<dim> > &qpoints = fe_v.get_quadrature_points();
        for (unsigned int q=0; q<quad_s.size(); ++q)
          {
            area += fe_v.JxW(q);
            center += fe_v.JxW(q)*qpoints[q];
          }
      }
    center /= area;
    global_info_file
        << area
        << " ";
    global_info_file
        << center
        << endl;
  }
}

// Determination of a vector of local dofs representing
//    the field <code>A_gamma</code>.

template <int dim>
void
ImmersedFEM<dim>::get_Agamma_values
(
  const FEValues<dim,dim> &fe_v_s,
  const vector< unsigned int > &dofs,
  const Vector<double> &xi,
  Vector<double> &local_A_gamma
)
{
  set_to_zero(local_A_gamma);

  unsigned int qsize = fe_v_s.get_quadrature().size();

  vector< vector< Tensor<1,dim> > >
  H(qsize, vector< Tensor<1,dim> >(dim));
  fe_v_s.get_function_gradients(xi, H);

  vector<Tensor<2,dim,double> > P (qsize, Tensor<2,dim,double>());
  vector<Tensor<2,dim,double> > tmp1;
  vector< vector<Tensor<2,dim,double> > > tmp2;

  get_Pe_F_and_DPeFT_dxi_values (
    fe_v_s,
    dofs,
    xi,
    false,
    P,
    tmp1,
    tmp2
  );

  for ( unsigned int qs = 0; qs < qsize; ++qs )
    {
      for (unsigned int k = 0; k < dofs.size(); ++k)
        {
          unsigned int comp_k = fe_s.system_to_component_index(k).first;


//Agamma = P:Grad_y
          local_A_gamma (k) +=
            P[qs][comp_k]*fe_v_s.shape_grad(k, qs)
            *fe_v_s.JxW(qs);
        }
    }
}

// Value of the product of the 1st Piola-Kirchhoff stress tensor and
// of the transpose of the deformation gradient at a given list of
// quadrature points on a cell of the immersed domain.

template <int dim>
void
ImmersedFEM<dim>::get_Pe_F_and_DPeFT_dxi_values (
  const FEValues<dim,dim> &fe_v_s,
  const vector< unsigned int > &dofs,
  const Vector<double> &xi,
  const bool update_jacobian,
  vector<Tensor<2,dim,double> > &Pe,
  vector<Tensor<2,dim,double> > &vec_F,
  vector< vector<Tensor<2,dim,double> > > &DPeFT_dxi
)
{
  vector< vector< Tensor<1,dim> > >
  H(Pe.size(), vector< Tensor<1,dim> >(dim));
  fe_v_s.get_function_gradients(xi, H);

  Tensor<2,dim,double> F;

  bool update_vecF = (vec_F.size()!= 0);


// The following variables are used when the
// <code>CircumferentialFiberModel</code> is used.
  Point<dim> p;
  Tensor<1, dim, double> etheta;
  Tensor<2, dim, double> etheta_op_etheta;
  Tensor<2, dim, double> tmp;

  for ( unsigned int qs = 0; qs < Pe.size(); ++qs )
    {
      for (unsigned int i=0; i <dim; ++i)
        {
          F[i] = H[qs][i];
          F[i][i] += 1.0;
        }

      if (update_vecF)
        vec_F[qs] = F;

      switch (par.material_model)
        {
        case IFEMParameters<dim>::INH_0:
          Pe[qs] = par.mu * ( F - transpose( invert(F) ) );
          if ( update_jacobian )
            {
              for ( unsigned int k = 0; k < fe_s.dofs_per_cell; ++k )
                {
                  DPeFT_dxi[qs][k] = 0.0;
                  unsigned int comp_k = fe_s.system_to_component_index(k).first;

                  for ( unsigned int i = 0; i < dim; ++i )
                    for ( unsigned int j = 0; j < dim; ++j )
                      {
                        if ( i == comp_k )
                          DPeFT_dxi[qs][k][i][j] += fe_v_s.shape_grad(k,qs)
                                                    * F[j];
                        if ( j == comp_k )
                          DPeFT_dxi[qs][k][i][j] += fe_v_s.shape_grad(k,qs)
                                                    * F[i];
                        DPeFT_dxi[qs][k][i][j] *= par.mu;
                      }
                }
            }
          break;
        case IFEMParameters<dim>::INH_1 :
          Pe[qs] = par.mu * F;
          if ( update_jacobian )
            {
              for ( unsigned int k = 0; k < fe_s.dofs_per_cell; ++k )
                {
                  DPeFT_dxi[qs][k] = 0.0;
                  unsigned int comp_k = fe_s.system_to_component_index(k).first;

                  for ( unsigned int i = 0; i < dim; ++i )
                    for ( unsigned int j = 0; j < dim; ++j )
                      {
                        if ( i == comp_k )
                          DPeFT_dxi[qs][k][i][j] += fe_v_s.shape_grad(k,qs)
                                                    * F[j];
                        if ( j == comp_k )
                          DPeFT_dxi[qs][k][i][j] += fe_v_s.shape_grad(k,qs)
                                                    * F[i];
                        DPeFT_dxi[qs][k][i][j] *= par.mu;
                      }
                }
            }
          break;
        case IFEMParameters<dim>::CircumferentialFiberModel:
          p = Point<dim>(fe_v_s.quadrature_point(qs) - par.ring_center);

// Find the unit vector along the tangential direction
          etheta[0]=-p[1]/p.norm();
          etheta[1]= p[0]/p.norm();


// Find the tensor product of etheta and etheta
          etheta_op_etheta = outer_product(etheta, etheta);
          Pe[qs] = contract<1,0> (F, etheta_op_etheta);
          Pe[qs] *= par.mu;
          if ( update_jacobian )
            {
              for ( unsigned int k = 0; k < fe_s.dofs_per_cell; ++k )
                {
                  DPeFT_dxi[qs][k] = 0.0;
                  unsigned int comp_k = fe_s.system_to_component_index(k).first;

                  for ( unsigned int i = 0; i < dim; ++i )
                    for ( unsigned int j = 0; j < dim; ++j )
                      {
                        if ( i == comp_k )
                          DPeFT_dxi[qs][k][i][j] += (fe_v_s.shape_grad(k,qs)
                                                     *etheta_op_etheta)*F[j];
                        if ( j == comp_k )
                          DPeFT_dxi[qs][k][i][j] += (fe_v_s.shape_grad(k,qs)
                                                     *etheta_op_etheta)* F[i];
                        DPeFT_dxi[qs][k][i][j] *= par.mu;
                      }
                }
            }
          break;
        default:
          break;
        }
    }
}

// Assemblage of the local residual in the global residual.

template <int dim>
void
ImmersedFEM<dim>::distribute_residual
(
  Vector<double> &residual,
  const vector<double> &local_res,
  const vector<unsigned int> &dofs_1,
  const unsigned int offset_1
)
{
  for (unsigned int i=0, wi=offset_1; i<dofs_1.size(); ++i,++wi)
    residual(dofs_1[i]) += local_res[wi];
}

// Assemblage of the local Jacobian in the global Jacobian.

template <int dim>
void
ImmersedFEM<dim>::distribute_jacobian
(
  SparseMatrix<double> &Jacobian,
  const FullMatrix<double> &local_Jac,
  const vector<unsigned int> &dofs_1,
  const vector<unsigned int> &dofs_2,
  const unsigned int offset_1,
  const unsigned int offset_2
)
{

  for (unsigned int i=0, wi=offset_1; i<dofs_1.size(); ++i,++wi)
    for (unsigned int j=0, wj=offset_2; j<dofs_2.size(); ++j,++wj)
      Jacobian.add(dofs_1[i],dofs_2[j],local_Jac(wi,wj));
}

// Application of constraints to the local residual and to the local
// contribution to the Jacobian.

template <int dim>
void
ImmersedFEM<dim>::apply_constraints
(
  vector<double> &local_res,
  FullMatrix<double> &local_jacobian,
  const Vector<double> &value_of_dofs,
  const vector<unsigned int> &dofs
)
{

  for (unsigned int i=0; i<dofs.size(); ++i)
    {
      map<unsigned int,double>::iterator it = par.boundary_values.find(dofs[i]);
      if (it != par.boundary_values.end() )
        {

// Setting the value of the residual equal to the difference between
// the current value and the the prescribed value.
          local_res[i] = scaling * ( value_of_dofs(dofs[i]) - it->second );
          if ( !local_jacobian.empty() )
            {

// Here we simply let the Jacobian know that the current dof is
// actually not a dof.
              for (unsigned int j=0; j<local_jacobian.n(); ++j)
                local_jacobian(i,j) = 0;
              local_jacobian(i,i) = scaling;
            }
        }


// Dealing with constraints concerning the pressure field.
      if (par.all_DBC && !par.fix_pressure)
        {
          if (dofs[i] == constraining_dof)
            {
              local_res[i] = 0;
              if ( !local_jacobian.empty() ) local_jacobian.add_row(i, -1, i);
            }
        }
    }
}

// Assemble the pressure constraint into the residual.
template <int dim>
void
ImmersedFEM<dim>::distribute_constraint_on_pressure
(
  Vector<double> &residual,
  const double average_pressure
)
{
  residual(constraining_dof) += average_pressure*scaling/area;
}

// Assemble the pressure constraint into the Jacobian.
template <int dim>
void
ImmersedFEM<dim>::distribute_constraint_on_pressure
(
  SparseMatrix<double> &jacobian,
  const vector<double> &pressure_coefficient,
  const vector<unsigned int> &dofs,
  const unsigned int offset
)
{
  for (unsigned int i=0, wi=offset; i<dofs.size(); ++i,++wi)
    jacobian.add(
      constraining_dof,
      dofs[i],
      pressure_coefficient[wi]*scaling/area
    );

}

// Determination of the dofs for the function
//    <code>M_gamma3_inv_A_gamma</code>.

template <int dim>
void
ImmersedFEM<dim>::localize
(
  Vector<double> &local_M_gamma3_inv_A_gamma,
  const Vector<double> &M_gamma3_inv_A_gamma,
  const vector<unsigned int> &dofs
)
{
  for (unsigned int i = 0; i < dofs.size(); ++i)
    local_M_gamma3_inv_A_gamma (i) = M_gamma3_inv_A_gamma(dofs[i]);
}

// Calculate the error for the equilibrium solution of corresponding
// to a ring with circumferential fibers.

template <int dim>
void
ImmersedFEM<dim>::calculate_error () const
{
  ExactSolutionRingWithFibers<dim> exact_sol(par);

  const ComponentSelectFunction<dim> pressure_mask(dim, dim+1);
  const ComponentSelectFunction<dim> velocity_mask(
    make_pair(0,dim),
    dim+1
  );

  const QIterated<dim> qiter_err(qtrapez, par.degree+1);

  Vector<float> difference_per_cell(tria_f.n_active_cells());


  VectorTools::integrate_difference (
    dh_f,
    current_xi.block(0),
    exact_sol,
    difference_per_cell,
    qiter_err,
    VectorTools::L2_norm,
    &velocity_mask
  );
  const double v_l2_norm = difference_per_cell.l2_norm();

  VectorTools::integrate_difference (
    dh_f,
    current_xi.block(0),
    exact_sol,
    difference_per_cell,
    qiter_err,
    VectorTools::H1_seminorm,
    &velocity_mask
  );
  const double v_h1_seminorm = difference_per_cell.l2_norm();

  VectorTools::integrate_difference (
    dh_f,
    current_xi.block(0),
    exact_sol,
    difference_per_cell,
    qiter_err,
    VectorTools::L2_norm,
    &pressure_mask
  );
  const double p_l2_norm = difference_per_cell.l2_norm();

  ofstream file_write;

  string filename;
  if (dgp_for_p)
    filename = "hello_world_error_norm_pFEDGP.dat";
  else
    filename = "hello_world_error_norm_pFEQ.dat";

  file_write.open(filename.c_str(), ios::out |ios::app);
  if (file_write.is_open())
    {
      file_write.unsetf(ios::floatfield);
      file_write
          << "- & "
          << setw(4)
          << tria_s.n_active_cells()
          << " & "
          << setw(6)
          << n_dofs_W
          << " & "
          << setw(4)
          << tria_f.n_active_cells()
          << " & "
          << setw(6)
          << n_dofs_up
          << scientific
          << setprecision(5)
          << " & "
          << setw(8)
          << v_l2_norm
          << " &-& "
          << setw(8)
          << v_h1_seminorm
          << " &-& "
          << setw(8)
          << p_l2_norm
          << " &- \\\\ \\hline"
          << endl;
    }
  file_write.close();

}

// Simple initialization to zero function templated on a generic type.

template <int dim>
template <class Type>
void ImmersedFEM<dim>::set_to_zero (Type &v) const
{
  v = 0;
}

// Simple initialization to zero function templated on a vector of
// generic type.
template <int dim>
template <class Type>
void ImmersedFEM<dim>::set_to_zero (vector<Type> &v) const
{
  for (unsigned int i = 0; i < v.size(); ++i) set_to_zero(v[i]);
}

// Simple initialization to zero function templated on a table of
// generic type.
template <int dim>
template <class Type>
void ImmersedFEM<dim>::set_to_zero (Table<2, Type> &v) const
{
  for (unsigned int i=0; i<v.size()[0]; ++i)
    for (unsigned int j=0; j<v.size()[1]; ++j) set_to_zero(v(i,j));
}

// Determination of the norm of a vector.
template <int dim>
double ImmersedFEM<dim>::norm(const vector<double> &v)
{
  double norm = 0;
  for ( unsigned int i = 0; i < v.size(); ++i) norm += v[i]*v[i];
  return norm = sqrt(norm);
}

// Explicit instantiations

template class ImmersedFEM<2>;
template class ImmersedFEM<3>;

