// ---------------------------------------------------------------------
//
// Copyright (C) 2011 - 2018 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE at
// the top level of the deal.II distribution.
//
// ---------------------------------------------------------------------


#ifndef dealii_matrix_free_h
#define dealii_matrix_free_h

#include <deal.II/base/aligned_vector.h>
#include <deal.II/base/exceptions.h>
#include <deal.II/base/quadrature.h>
#include <deal.II/base/vectorization.h>
#include <deal.II/base/thread_local_storage.h>
#include <deal.II/base/template_constraints.h>
#include <deal.II/fe/fe.h>
#include <deal.II/fe/mapping.h>
#include <deal.II/fe/mapping_q1.h>
#include <deal.II/lac/vector_operation.h>
#include <deal.II/lac/la_parallel_vector.h>
#include <deal.II/lac/block_vector_base.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/grid/grid_tools.h>
#include <deal.II/hp/dof_handler.h>
#include <deal.II/hp/q_collection.h>
#include <deal.II/matrix_free/task_info.h>
#include <deal.II/matrix_free/shape_info.h>
#include <deal.II/matrix_free/dof_info.h>
#include <deal.II/matrix_free/mapping_info.h>

#include <stdlib.h>
#include <memory>
#include <limits>
#include <list>


DEAL_II_NAMESPACE_OPEN



/**
 * This class collects all the data that is stored for the matrix free
 * implementation. The storage scheme is tailored towards several loops
 * performed with the same data, i.e., typically doing many matrix-vector
 * products or residual computations on the same mesh. The class is used in
 * step-37 and step-48.
 *
 * This class does not implement any operations involving finite element basis
 * functions, i.e., regarding the operation performed on the cells. For these
 * operations, the class FEEvaluation is designed to use the data collected in
 * this class.
 *
 * The stored data can be subdivided into three main components:
 *
 * - DoFInfo: It stores how local degrees of freedom relate to global degrees
 * of freedom. It includes a description of constraints that are evaluated as
 * going through all local degrees of freedom on a cell.
 *
 * - MappingInfo: It stores the transformations from real to unit cells that
 * are necessary in order to build derivatives of finite element functions and
 * find location of quadrature weights in physical space.
 *
 * - ShapeInfo: It contains the shape functions of the finite element,
 * evaluated on the unit cell.
 *
 * Besides the initialization routines, this class implements only a single
 * operation, namely a loop over all cells (cell_loop()). This loop is
 * scheduled in such a way that cells that share degrees of freedom are not
 * worked on simultaneously, which implies that it is possible to write to
 * vectors (or matrices) in parallel without having to explicitly synchronize
 * access to these vectors and matrices. This class does not implement any
 * shape values, all it does is to cache the respective data. To implement
 * finite element operations, use the class FEEvaluation (or some of the
 * related classes).
 *
 * This class traverses the cells in a different order than the usual
 * Triangulation class in deal.II, in order to be flexible with respect to
 * parallelization in shared memory and vectorization.
 *
 * Vectorization is implemented by merging several topological cells into one
 * so-called macro cell. This enables the application of all cell-related
 * operations for several cells with one CPU instruction and is one of the
 * main features of this framework.
 *
 * For details on usage of this class, see the description of FEEvaluation or
 * the @ref matrixfree "matrix-free module".
 *
 * @ingroup matrixfree
 *
 * @author Katharina Kormann, Martin Kronbichler, 2010, 2011
 */

template <int dim, typename Number=double>
class MatrixFree : public Subscriptor
{
public:
  /**
   * A typedef for the underlying number type specified by the template
   * argument.
   */
  typedef Number            value_type;

  /**
   * The dimension set by the template argument `dim`.
   */
  static const unsigned int dimension = dim;

  /**
   * Collects the options for initialization of the MatrixFree class. The
   * first parameter specifies the MPI communicator to be used, the second the
   * parallelization options in shared memory (task-based parallelism, where
   * one can choose between no parallelism and three schemes that avoid that
   * cells with access to the same vector entries are accessed
   * simultaneously), the third with the block size for task parallel
   * scheduling, the fourth the update flags that should be stored by this
   * class.
   *
   * The fifth parameter specifies the level in the triangulation from which
   * the indices are to be used. If the level is set to
   * numbers::invalid_unsigned_int, the active cells are traversed, and
   * otherwise the cells in the given level. This option has no effect in case
   * a DoFHandler or hp::DoFHandler is given.
   *
   * The parameter @p initialize_plain_indices indicates whether the DoFInfo
   * class should also allow for access to vectors without resolving
   * constraints.
   *
   * The two parameters `initialize_indices` and `initialize_mapping` allow
   * the user to disable some of the initialization processes. For example, if
   * only the scheduling that avoids touching the same vector/matrix indices
   * simultaneously is to be found, the mapping needs not be
   * initialized. Likewise, if the mapping has changed from one iteration to
   * the next but the topology has not (like when using a deforming mesh with
   * MappingQEulerian), it suffices to initialize the mapping only.
   *
   * The two parameters `cell_vectorization_categories` and
   * `cell_vectorization_categories_strict` control the formation of batches
   * for vectorization over several cells. It is used implicitly when working
   * with hp adaptivity but can also be useful in other contexts, such as in
   * local time stepping where one would like to control which elements
   * together form a batch of cells. The array `cell_vectorization_categories`
   * is accessed by the number given by cell->active_cell_index() when working
   * on the active cells with `level_mg_handler` set to `-1` and by
   * cell->index() for the level cells. By default, the different categories
   * in `cell_vectorization_category` can be mixed and the algorithm is
   * allowed to merge lower category numbers with the next higher categories
   * if it is necessary inside the algorithm, in order to avoid partially
   * filled SIMD lanes as much as possible. This gives a better utilization of
   * the vectorization but might need special treatment, in particular for
   * face integrals. If set to @p true, the algorithm will instead keep
   * different categories separate and not mix them in a single vectorized
   * array.
   */
  struct AdditionalData
  {
    /**
     * Collects options for task parallelism. See the documentation of the
     * member variable MatrixFree::AdditionalData::tasks_parallel_scheme for a
     * thorough description.
     */
    enum TasksParallelScheme
    {
      /**
       * Perform application in serial.
       */
      none = internal::MatrixFreeFunctions::TaskInfo::none,
      /**
       * Partition the cells into two levels and afterwards form chunks.
       */
      partition_partition = internal::MatrixFreeFunctions::TaskInfo::partition_partition,
      /**
       * Partition on the global level and color cells within the partitions.
       */
      partition_color = internal::MatrixFreeFunctions::TaskInfo::partition_color,
      /**
       * Use the traditional coloring algorithm: this is like
       * TasksParallelScheme::partition_color, but only uses one partition.
       */
      color = internal::MatrixFreeFunctions::TaskInfo::color
    };

    /**
     * Constructor for AdditionalData.
     */
    AdditionalData (const TasksParallelScheme tasks_parallel_scheme = partition_partition,
                    const unsigned int        tasks_block_size   = 0,
                    const UpdateFlags         mapping_update_flags  = update_gradients | update_JxW_values,
                    const UpdateFlags         mapping_update_flags_boundary_faces = update_default,
                    const UpdateFlags         mapping_update_flags_inner_faces = update_default,
                    const UpdateFlags         mapping_update_flags_faces_by_cells = update_default,
                    const unsigned int        level_mg_handler = numbers::invalid_unsigned_int,
                    const bool                store_plain_indices = true,
                    const bool                initialize_indices = true,
                    const bool                initialize_mapping = true,
                    const bool                overlap_communication_computation = true,
                    const bool                hold_all_faces_to_owned_cells = false,
                    const bool                cell_vectorization_categories_strict = false)
      :
      tasks_parallel_scheme (tasks_parallel_scheme),
      tasks_block_size      (tasks_block_size),
      mapping_update_flags  (mapping_update_flags),
      mapping_update_flags_boundary_faces (mapping_update_flags_boundary_faces),
      mapping_update_flags_inner_faces (mapping_update_flags_inner_faces),
      mapping_update_flags_faces_by_cells (mapping_update_flags_faces_by_cells),
      level_mg_handler      (level_mg_handler),
      store_plain_indices   (store_plain_indices),
      initialize_indices    (initialize_indices),
      initialize_mapping    (initialize_mapping),
      overlap_communication_computation(overlap_communication_computation),
      hold_all_faces_to_owned_cells(hold_all_faces_to_owned_cells),
      cell_vectorization_categories_strict(cell_vectorization_categories_strict)
    {};

    /**
     * Set the scheme for task parallelism. There are four options available.
     * If set to @p none, the operator application is done in serial without
     * shared memory parallelism. If this class is used together with MPI and
     * MPI is also used for parallelism within the nodes, this flag should be
     * set to @p none. The default value is @p partition_partition, i.e. we
     * actually use multithreading with the first option described below.
     *
     * The first option @p partition_partition is to partition the cells on
     * two levels in onion-skin-like partitions and forming chunks of
     * tasks_block_size after the partitioning. The partitioning finds sets of
     * independent cells that enable working in parallel without accessing the
     * same vector entries at the same time.
     *
     * The second option @p partition_color is to use a partition on the
     * global level and color cells within the partitions (where all chunks
     * within a color are independent). Here, the subdivision into chunks of
     * cells is done before the partitioning, which might give worse
     * partitions but better cache performance if degrees of freedom are not
     * renumbered.
     *
     * The third option @p color is to use a traditional algorithm of coloring
     * on the global level. This scheme is a special case of the second option
     * where only one partition is present. Note that for problems with
     * hanging nodes, there are quite many colors (50 or more in 3D), which
     * might degrade parallel performance (bad cache behavior, many
     * synchronization points).
     */
    TasksParallelScheme tasks_parallel_scheme;

    /**
     * Set the number of so-called macro cells that should form one
     * partition. If zero size is given, the class tries to find a good size
     * for the blocks based on MultithreadInfo::n_threads() and the number of
     * cells present. Otherwise, the given number is used. If the given number
     * is larger than one third of the number of total cells, this means no
     * parallelism. Note that in the case vectorization is used, a macro cell
     * consists of more than one physical cell.
     */
    unsigned int        tasks_block_size;

    /**
     * This flag determines the mapping data on cells that is cached. This
     * class can cache data needed for gradient computations (inverse
     * Jacobians), Jacobian determinants (JxW), quadrature points as well as
     * data for Hessians (derivative of Jacobians). By default, only data for
     * gradients and Jacobian determinants times quadrature weights, JxW, are
     * cached. If quadrature points or second derivatives are needed, they
     * must be specified by this field (even though second derivatives might
     * still be evaluated on Cartesian cells without this option set here,
     * since there the Jacobian describes the mapping completely).
     */
    UpdateFlags         mapping_update_flags;

    /**
     * This flag determines the mapping data on boundary faces to be
     * cached. Note that MatrixFree uses a separate loop layout for face
     * integrals in order to effectively vectorize also in the case of hanging
     * nodes (which require different subface settings on the two sides) or
     * some cells in the batch of a VectorizedArray of cells that are adjacent
     * to the boundary and others that are not.
     *
     * If set to a value different from update_general (default), the face
     * information is explicitly built. Currently, MatrixFree supports to
     * cache the following data on faces: inverse Jacobians, Jacobian
     * determinants (JxW), quadrature points, data for Hessians (derivative of
     * Jacobians), and normal vectors.
     */
    UpdateFlags         mapping_update_flags_boundary_faces;

    /**
     * This flag determines the mapping data on interior faces to be
     * cached. Note that MatrixFree uses a separate loop layout for face
     * integrals in order to effectively vectorize also in the case of hanging
     * nodes (which require different subface settings on the two sides) or
     * some cells in the batch of a VectorizedArray of cells that are adjacent
     * to the boundary and others that are not.
     *
     * If set to a value different from update_general (default), the face
     * information is explicitly built. Currently, MatrixFree supports to
     * cache the following data on faces: inverse Jacobians, Jacobian
     * determinants (JxW), quadrature points, data for Hessians (derivative of
     * Jacobians), and normal vectors.
     */
    UpdateFlags         mapping_update_flags_inner_faces;

    /**
     * This flag determines the mapping data for faces in a different layout
     * with respect to vectorizations. Whereas
     * `mapping_update_flags_inner_faces` and
     * `mapping_update_flags_boundary_faces` trigger building the data in a
     * face-centric way with proper vectorization, the current data field
     * attaches the face information to the cells and their way of
     * vectorization. This is only needed in special situations, as for
     * example for block-Jacobi methods where the full operator to a cell
     * including its faces are evaluated. This data is accessed by
     * <code>FEFaceEvaluation::reinit(cell_batch_index,
     * face_number)</code>. However, currently no coupling terms to neighbors
     * can be computed with this approach because the neighbors are not laid
     * out by the VectorizedArray data layout with an
     * array-of-struct-of-array-type data structures.
     *
     * Note that you should only compute this data field in case you really
     * need it as it more than doubles the memory required by the mapping data
     * on faces.
     *
     * If set to a value different from update_general (default), the face
     * information is explicitly built. Currently, MatrixFree supports to
     * cache the following data on faces: inverse Jacobians, Jacobian
     * determinants (JxW), quadrature points, data for Hessians (derivative of
     * Jacobians), and normal vectors.
     */
    UpdateFlags         mapping_update_flags_faces_by_cells;

    /**
     * This option can be used to define whether we work on a certain level of
     * the mesh, and not the active cells. If set to invalid_unsigned_int
     * (which is the default value), the active cells are gone through,
     * otherwise the level given by this parameter. Note that if you specify
     * to work on a level, its dofs must be distributed by using
     * <code>dof_handler.distribute_mg_dofs(fe);</code>.
     */
    unsigned int        level_mg_handler;

    /**
     * Controls whether to allow reading from vectors without resolving
     * constraints, i.e., just read the local values of the vector. By
     * default, this option is disabled, so if you want to use
     * FEEvaluationBase::read_dof_values_plain, this flag needs to be set.
     */
    bool                store_plain_indices;

    /**
     * Option to control whether the indices stored in the DoFHandler
     * should be read and the pattern for task parallelism should be
     * set up in the initialize method of MatrixFree. The default
     * value is true. Can be disabled in case the mapping should be
     * recomputed (e.g. when using a deforming mesh described through
     * MappingEulerian) but the topology of cells has remained the
     * same.
     */
    bool                initialize_indices;

    /**
     * Option to control whether the mapping information should be
     * computed in the initialize method of MatrixFree. The default
     * value is true. Can be disabled when only some indices should be
     * set up (e.g. when only a set of independent cells should be
     * computed).
     */
    bool                initialize_mapping;

    /**
     * Option to control whether the loops should overlap communications and
     * computations as far as possible in case the vectors passed to the loops
     * support non-blocking data exchange. In most situations, overlapping is
     * faster in case the amount of data to be sent is more than a few
     * kilobytes. If less data is sent, the communication is latency bound on
     * most clusters (point-to-point latency is around 1 microsecond on good
     * clusters by 2016 standards). Depending on the MPI implementation and
     * the fabric, it may be faster to not overlap and wait for the data to
     * arrive. The default is true, i.e., communication and computation are
     * overlapped.
     **/
    bool                overlap_communication_computation;

    /**
     * By default, the face part will only hold those faces (and ghost
     * elements behind faces) that are going to be processed locally. In case
     * MatrixFree should have access to all neighbors on locally owned cells,
     * this option enables adding the respective faces at the end of the face
     * range.
     **/
    bool                hold_all_faces_to_owned_cells;

    /**
     * This data structure allows to assign a fraction of cells to different
     * categories when building the information for vectorization. It is used
     * implicitly when working with hp adaptivity but can also be useful in
     * other contexts, such as in local time stepping where one would like to
     * control which elements together form a batch of cells.
     *
     * This array is accessed by the number given by cell->active_cell_index()
     * when working on the active cells with @p level_mg_handler set to -1 and
     * by cell->index() for the level cells.
     *
     * @note This field is empty upon construction of AdditionalData. It is
     * the responsibility of the user to resize this field to
     * `triangulation.n_active_cells()` or `triangulation.n_cells(level)` when
     * filling data.
     */
    std::vector<unsigned int> cell_vectorization_category;

    /**
     * By default, the different categories in @p cell_vectorization_category
     * can be mixed and the algorithm is allowed to merge lower categories with
     * the next higher categories if it is necessary inside the algorithm. This
     * gives a better utilization of the vectorization but might need special
     * treatment, in particular for face integrals. If set to @p true, the
     * algorithm will instead keep different categories separate and not mix
     * them in a single vectorized array.
     */
    bool cell_vectorization_categories_strict;
  };

  /**
   * @name 1: Construction and initialization
   */
  //@{
  /**
   * Default empty constructor. Does nothing.
   */
  MatrixFree ();

  /**
   * Copy constructor, calls copy_from
   */
  MatrixFree (const MatrixFree<dim,Number> &other);

  /**
   * Destructor.
   */
  ~MatrixFree() = default;

  /**
   * Extracts the information needed to perform loops over cells. The
   * DoFHandler and ConstraintMatrix describe the layout of degrees of
   * freedom, the DoFHandler and the mapping describe the transformations from
   * unit to real cell, and the finite element underlying the DoFHandler
   * together with the quadrature formula describe the local operations. Note
   * that the finite element underlying the DoFHandler must either be scalar
   * or contain several copies of the same element. Mixing several different
   * elements into one FESystem is not allowed. In that case, use the
   * initialization function with several DoFHandler arguments.
   */
  template <typename DoFHandlerType, typename QuadratureType>
  void reinit (const Mapping<dim>     &mapping,
               const DoFHandlerType   &dof_handler,
               const ConstraintMatrix &constraint,
               const QuadratureType   &quad,
               const AdditionalData    additional_data = AdditionalData());

  /**
   * Initializes the data structures. Same as above, but using a $Q_1$
   * mapping.
   */
  template <typename DoFHandlerType, typename QuadratureType>
  void reinit (const DoFHandlerType   &dof_handler,
               const ConstraintMatrix &constraint,
               const QuadratureType   &quad,
               const AdditionalData    additional_data = AdditionalData());

  /**
   * Same as above.
   *
   * @deprecated Setting the index set specifically is not supported any
   * more. Use the reinit function without index set argument to choose the
   * one provided by DoFHandler::locally_owned_dofs().
   */
  template <typename DoFHandlerType, typename QuadratureType>
  DEAL_II_DEPRECATED
  void reinit (const Mapping<dim>     &mapping,
               const DoFHandlerType   &dof_handler,
               const ConstraintMatrix &constraint,
               const IndexSet         &locally_owned_dofs,
               const QuadratureType   &quad,
               const AdditionalData    additional_data = AdditionalData());

  /**
   * Extracts the information needed to perform loops over cells. The
   * DoFHandler and ConstraintMatrix describe the layout of degrees of
   * freedom, the DoFHandler and the mapping describe the transformations from
   * unit to real cell, and the finite element underlying the DoFHandler
   * together with the quadrature formula describe the local operations. As
   * opposed to the scalar case treated with the other initialization
   * functions, this function allows for problems with two or more different
   * finite elements. The DoFHandlers to each element must be passed as
   * pointers to the initialization function. Note that the finite element
   * underlying an DoFHandler must either be scalar or contain several copies
   * of the same element. Mixing several different elements into one @p
   * FE_System is not allowed.
   *
   * This function also allows for using several quadrature formulas, e.g.
   * when the description contains independent integrations of elements of
   * different degrees. However, the number of different quadrature formulas
   * can be sets independently from the number of DoFHandlers, when several
   * elements are always integrated with the same quadrature formula.
   */
  template <typename DoFHandlerType, typename QuadratureType>
  void reinit (const Mapping<dim>                          &mapping,
               const std::vector<const DoFHandlerType *>   &dof_handler,
               const std::vector<const ConstraintMatrix *> &constraint,
               const std::vector<QuadratureType>           &quad,
               const AdditionalData                         additional_data = AdditionalData());

  /**
   * Initializes the data structures. Same as above, but  using a $Q_1$
   * mapping.
   */
  template <typename DoFHandlerType, typename QuadratureType>
  void reinit (const std::vector<const DoFHandlerType *>   &dof_handler,
               const std::vector<const ConstraintMatrix *> &constraint,
               const std::vector<QuadratureType>           &quad,
               const AdditionalData                         additional_data = AdditionalData());

  /**
   * Same as above.
   *
   * @deprecated Setting the index set specifically is not supported any
   * more. Use the reinit function without index set argument to choose the
   * one provided by DoFHandler::locally_owned_dofs().
   */
  template <typename DoFHandlerType, typename QuadratureType>
  DEAL_II_DEPRECATED
  void reinit (const Mapping<dim>                          &mapping,
               const std::vector<const DoFHandlerType *>   &dof_handler,
               const std::vector<const ConstraintMatrix *> &constraint,
               const std::vector<IndexSet>                 &locally_owned_set,
               const std::vector<QuadratureType>           &quad,
               const AdditionalData                        additional_data = AdditionalData());

  /**
   * Initializes the data structures. Same as before, but now the index set
   * description of the locally owned range of degrees of freedom is taken
   * from the DoFHandler. Moreover, only a single quadrature formula is used,
   * as might be necessary when several components in a vector-valued problem
   * are integrated together based on the same quadrature formula.
   */
  template <typename DoFHandlerType, typename QuadratureType>
  void reinit (const Mapping<dim>                          &mapping,
               const std::vector<const DoFHandlerType *>   &dof_handler,
               const std::vector<const ConstraintMatrix *> &constraint,
               const QuadratureType                        &quad,
               const AdditionalData                         additional_data = AdditionalData());

  /**
   * Initializes the data structures. Same as above, but  using a $Q_1$
   * mapping.
   */
  template <typename DoFHandlerType, typename QuadratureType>
  void reinit (const std::vector<const DoFHandlerType *>   &dof_handler,
               const std::vector<const ConstraintMatrix *> &constraint,
               const QuadratureType                        &quad,
               const AdditionalData                         additional_data = AdditionalData());

  /**
   * Copy function. Creates a deep copy of all data structures. It is usually
   * enough to keep the data for different operations once, so this function
   * should not be needed very often.
   */
  void copy_from (const MatrixFree<dim,Number> &matrix_free_base);

  /**
   * Clear all data fields and brings the class into a condition similar to
   * after having called the default constructor.
   */
  void clear();

  //@}

  /**
   * @name 2: Loop over cells
   */
  //@{
  /**
   * This method runs the loop over all cells (in parallel) and performs the
   * MPI data exchange on the source vector and destination vector.
   *
   * @param cell_operation `std::function` with the signature `cell_operation
   * (const MatrixFree<dim,Number> &, OutVector &, InVector &,
   * std::pair<unsigned int,unsigned int> &)` where the first argument passes
   * the data of the calling class and the last argument defines the range of
   * cells which should be worked on (typically more than one cell should be
   * worked on in order to reduce overheads).  One can pass a pointer to an
   * object in this place if it has an `operator()` with the
   * correct set of arguments since such a pointer can be converted to the
   * function object.
   *
   * @param dst Destination vector holding the result. If the vector is of
   * type LinearAlgebra::distributed::Vector (or composite objects thereof
   * such as LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::compress() at the end of the call
   * internally.
   *
   * @param src Input vector. If the vector is of type
   * LinearAlgebra::distributed::Vector (or composite objects thereof such as
   * LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::update_ghost_values() at the start of
   * the call internally to make sure all necessary data is locally
   * available. Note, however, that the vector is reset to its original state
   * at the end of the loop, i.e., if the vector was not ghosted upon entry of
   * the loop, it will not be ghosted upon finishing the loop.
   *
   * @param zero_dst_vector If this flag is set to `true`, the vector `dst`
   * will be set to zero inside the loop. Use this case in case you perform a
   * typical `vmult()` operation on a matrix object, as it will typically be
   * faster than calling `dst = 0;` before the loop separately. This is
   * because the vector entries are set to zero only on subranges of the
   * vector, making sure that the vector entries stay in caches as much as
   * possible.
   */
  template <typename OutVector, typename InVector>
  void cell_loop (const std::function<void (const MatrixFree<dim,Number> &,
                                            OutVector &,
                                            const InVector &,
                                            const std::pair<unsigned int,
                                            unsigned int> &)> &cell_operation,
                  OutVector      &dst,
                  const InVector &src,
                  const bool      zero_dst_vector = false) const;

  /**
   * This is the second variant to run the loop over all cells, now providing
   * a function pointer to a member function of class `CLASS`. This method
   * obviates the need to call std::bind to bind the class into the given
   * function in case the local function needs to access data in the class
   * (i.e., it is a non-static member function).
   *
   * @param cell_operation Pointer to member function of `CLASS` with the
   * signature `cell_operation (const MatrixFree<dim,Number> &, OutVector &,
   * InVector &, std::pair<unsigned int,unsigned int> &)` where the first
   * argument passes the data of the calling class and the last argument
   * defines the range of cells which should be worked on (typically more than
   * one cell should be worked on in order to reduce overheads).
   *
   * @param owning class The object which provides the `cell_operation`
   * call. To be compatible with this interface, the class must allow to call
   * `owning_class->cell_operation(...)`.
   *
   * @param dst Destination vector holding the result. If the vector is of
   * type LinearAlgebra::distributed::Vector (or composite objects thereof
   * such as LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::compress() at the end of the call
   * internally.
   *
   * @param src Input vector. If the vector is of type
   * LinearAlgebra::distributed::Vector (or composite objects thereof such as
   * LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::update_ghost_values() at the start of
   * the call internally to make sure all necessary data is locally
   * available. Note, however, that the vector is reset to its original state
   * at the end of the loop, i.e., if the vector was not ghosted upon entry of
   * the loop, it will not be ghosted upon finishing the loop.
   *
   * @param zero_dst_vector If this flag is set to `true`, the vector `dst`
   * will be set to zero inside the loop. Use this case in case you perform a
   * typical `vmult()` operation on a matrix object, as it will typically be
   * faster than calling `dst = 0;` before the loop separately. This is
   * because the vector entries are set to zero only on subranges of the
   * vector, making sure that the vector entries stay in caches as much as
   * possible.
   */
  template <typename CLASS, typename OutVector, typename InVector>
  void cell_loop (void (CLASS::*cell_operation)(const MatrixFree &,
                                                OutVector &,
                                                const InVector &,
                                                const std::pair<unsigned int,
                                                unsigned int> &)const,
                  const CLASS    *owning_class,
                  OutVector      &dst,
                  const InVector &src,
                  const bool      zero_dst_vector = false) const;

  /**
   * Same as above, but for class member functions which are non-const.
   */
  template <typename CLASS, typename OutVector, typename InVector>
  void cell_loop (void (CLASS::*cell_operation)(const MatrixFree &,
                                                OutVector &,
                                                const InVector &,
                                                const std::pair<unsigned int,
                                                unsigned int> &),
                  CLASS          *owning_class,
                  OutVector      &dst,
                  const InVector &src,
                  const bool      zero_dst_vector = false) const;

  /**
   * This class defines the type of data access for face integrals that is
   * passed on to the `update_ghost_values` and `compress` functions of the
   * parallel vectors, with the purpose of being able to reduce the amount of
   * data that must be exchanged. The data exchange is a real bottleneck in
   * particular for high-degree DG methods, therefore a more restrictive way
   * of exchange is clearly beneficial. Note that this selection applies to
   * FEFaceEvaluation objects assigned to the exterior side of cells accessing
   * `FaceToCellTopology::exterior_cells` only; all <i>interior</i> objects
   * are available in any case.
   */
  enum class DataAccessOnFaces
  {
    /**
     * The loop does not involve any FEFaceEvaluation access into neighbors,
     * as is the case with only boundary integrals (but no interior face
     * integrals) or when doing mass matrices in a MatrixFree::cell_loop()
     * like setup.
     */
    none,

    /**
     * The loop does only involve FEFaceEvaluation access into neighbors by
     * function values, such as `FEFaceEvaluation::gather_evaluate(src, true,
     * false);`, but no access to shape function derivatives (which typically
     * need to access more data). For FiniteElement types where only some of
     * the shape functions have support on a face, such as an FE_DGQ element
     * with Lagrange polynomials with nodes on the element surface, the data
     * exchange is reduced from `(k+1)^dim` to `(k+1)^(dim-1)`.
     */
    values,

    /**
     * The loop does involve FEFaceEvaluation access into neighbors by
     * function values and gradients, but no second derivatives, such as
     * `FEFaceEvaluation::gather_evaluate(src, true, true);`. For
     * FiniteElement types where only some of the shape functions have
     * non-zero value and first derivative on a face, such as an FE_DGQHermite
     * element, the data exchange is reduced, e.g. from `(k+1)^dim` to
     * `2(k+1)^(dim-1)`. Note that for bases that do not have this special
     * property, the full neighboring data is sent anyway.
     */
    gradients,

    /**
     * General setup where the user does not want to make a restriction. This
     * is typically more expensive than the other options, but also the most
     * conservative one because the full data of elements behind the faces to
     * be computed locally will be exchanged.
     */
    unspecified
  };

  /**
   * This method runs a loop over all cells (in parallel) and performs the MPI
   * data exchange on the source vector and destination vector. As opposed to
   * the other variants that only runs a function on cells, this method also
   * takes as arguments a function for the interior faces and for the boundary
   * faces, respectively.
   *
   * @param cell_operation `std::function` with the signature `cell_operation
   * (const MatrixFree<dim,Number> &, OutVector &, InVector &,
   * std::pair<unsigned int,unsigned int> &)` where the first argument passes
   * the data of the calling class and the last argument defines the range of
   * cells which should be worked on (typically more than one cell should be
   * worked on in order to reduce overheads).  One can pass a pointer to an
   * object in this place if it has an <code>operator()</code> with the
   * correct set of arguments since such a pointer can be converted to the
   * function object.
   *
   * @param face_operation `std::function` with the signature `face_operation
   * (const MatrixFree<dim,Number> &, OutVector &, InVector &,
   * std::pair<unsigned int,unsigned int> &)` in analogy to `cell_operation`,
   * but now the part associated to the work on interior faces. Note that the
   * MatrixFree framework treats periodic faces as interior ones, so they will
   * be assigned their correct neighbor after applying periodicity constraints
   * within the face_operation calls.
   *
   * @param face_operation `std::function` with the signature
   * `boundary_operation (const MatrixFree<dim,Number> &, OutVector &,
   * InVector &, std::pair<unsigned int,unsigned int> &)` in analogy to
   * `cell_operation` and `face_operation`, but now the part associated to the
   * work on boundary faces. Boundary faces are separated by their
   * `boundary_id` and it is possible to query that id using
   * MatrixFree::get_boundary_id(). Note that both interior and faces use the
   * same numbering, and faces in the interior are assigned lower numbers than
   * the boundary faces.
   *
   * @param dst Destination vector holding the result. If the vector is of
   * type LinearAlgebra::distributed::Vector (or composite objects thereof
   * such as LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::compress() at the end of the call
   * internally.
   *
   * @param src Input vector. If the vector is of type
   * LinearAlgebra::distributed::Vector (or composite objects thereof such as
   * LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::update_ghost_values() at the start of
   * the call internally to make sure all necessary data is locally
   * available. Note, however, that the vector is reset to its original state
   * at the end of the loop, i.e., if the vector was not ghosted upon entry of
   * the loop, it will not be ghosted upon finishing the loop.
   *
   * @param zero_dst_vector If this flag is set to `true`, the vector `dst`
   * will be set to zero inside the loop. Use this case in case you perform a
   * typical `vmult()` operation on a matrix object, as it will typically be
   * faster than calling `dst = 0;` before the loop separately. This is
   * because the vector entries are set to zero only on subranges of the
   * vector, making sure that the vector entries stay in caches as much as
   * possible.
   *
   * @param dst_vector_face_access Set the type of access into the vector
   * `dst` that will happen inside the body of the @p face_operation
   * function. As explained in the description of the DataAccessOnFaces
   * struct, the purpose of this selection is to reduce the amount of data
   * that must be exchanged over the MPI network (or via `memcpy` if within
   * the shared memory region of a node) to gain performance. Note that there
   * is no way to communicate this setting with the FEFaceEvaluation class,
   * therefore this selection must be made at this site in addition to what is
   * implemented inside the `face_operation` function. As a consequence, there
   * is also no way to check that the setting passed to this call is
   * consistent with what is later done by `FEFaceEvaluation`, and it is the
   * user's responsibility to ensure correctness of data.
   *
   * @param src_vector_face_access Set the type of access into the vector
   * `src` that will happen inside the body of the @p face_operation function,
   * in analogy to `dst_vector_face_access`.
   */
  template <typename OutVector, typename InVector>
  void loop (const std::function<void (const MatrixFree<dim,Number> &,
                                       OutVector &,
                                       const InVector &,
                                       const std::pair<unsigned int,
                                       unsigned int> &)> &cell_operation,
             const std::function<void (const MatrixFree<dim,Number> &,
                                       OutVector &,
                                       const InVector &,
                                       const std::pair<unsigned int,
                                       unsigned int> &)> &face_operation,
             const std::function<void (const MatrixFree<dim,Number> &,
                                       OutVector &,
                                       const InVector &,
                                       const std::pair<unsigned int,
                                       unsigned int> &)> &boundary_operation,
             OutVector      &dst,
             const InVector &src,
             const bool      zero_dst_vector = false,
             const DataAccessOnFaces dst_vector_face_access = DataAccessOnFaces::unspecified,
             const DataAccessOnFaces src_vector_face_access = DataAccessOnFaces::unspecified) const;

  /**
   * This is the second variant to run the loop over all cells, interior
   * faces, and boundary faces, now providing three function pointers to
   * member functions of class @p CLASS with the signature <code>operation
   * (const MatrixFree<dim,Number> &, OutVector &, InVector &,
   * std::pair<unsigned int,unsigned int>&)const</code>. This method obviates
   * the need to call std::bind to bind the class into the given
   * function in case the local function needs to access data in the class
   * (i.e., it is a non-static member function).
   *
   * @param cell_operation Pointer to member function of `CLASS` with the
   * signature `cell_operation (const MatrixFree<dim,Number> &, OutVector &,
   * InVector &, std::pair<unsigned int,unsigned int> &)` where the first
   * argument passes the data of the calling class and the last argument
   * defines the range of cells which should be worked on (typically more than
   * one cell should be worked on in order to reduce overheads). Note that the
   * loop will typically split the `cell_range` into smaller pieces and work
   * on `cell_operation`, `face_operation`, and `boundary_operation`
   * alternately, in order to increase the potential reuse of vector entries
   * in caches.
   *
   * @param face_operation Pointer to member function of `CLASS` with the
   * signature `face_operation (const MatrixFree<dim,Number> &, OutVector &,
   * InVector &, std::pair<unsigned int,unsigned int> &)` in analogy to
   * `cell_operation`, but now the part associated to the work on interior
   * faces. Note that the MatrixFree framework treats periodic faces as
   * interior ones, so they will be assigned their correct neighbor after
   * applying periodicity constraints within the face_operation calls.
   *
   * @param face_operation Pointer to member function of `CLASS` with the
   * signature `boundary_operation (const MatrixFree<dim,Number> &, OutVector
   * &, InVector &, std::pair<unsigned int,unsigned int> &)` in analogy to
   * `cell_operation` and `face_operation`, but now the part associated to the
   * work on boundary faces. Boundary faces are separated by their
   * `boundary_id` and it is possible to query that id using
   * MatrixFree::get_boundary_id(). Note that both interior and faces use the
   * same numbering, and faces in the interior are assigned lower numbers than
   * the boundary faces.
   *
   * @param dst Destination vector holding the result. If the vector is of
   * type LinearAlgebra::distributed::Vector (or composite objects thereof
   * such as LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::compress() at the end of the call
   * internally.
   *
   * @param src Input vector. If the vector is of type
   * LinearAlgebra::distributed::Vector (or composite objects thereof such as
   * LinearAlgebra::distributed::BlockVector), the loop calls
   * LinearAlgebra::distributed::Vector::update_ghost_values() at the start of
   * the call internally to make sure all necessary data is locally
   * available. Note, however, that the vector is reset to its original state
   * at the end of the loop, i.e., if the vector was not ghosted upon entry of
   * the loop, it will not be ghosted upon finishing the loop.
   *
   * @param zero_dst_vector If this flag is set to `true`, the vector `dst`
   * will be set to zero inside the loop. Use this case in case you perform a
   * typical `vmult()` operation on a matrix object, as it will typically be
   * faster than calling `dst = 0;` before the loop separately. This is
   * because the vector entries are set to zero only on subranges of the
   * vector, making sure that the vector entries stay in caches as much as
   * possible.
   *
   * @param dst_vector_face_access Set the type of access into the vector
   * `dst` that will happen inside the body of the @p face_operation
   * function. As explained in the description of the DataAccessOnFaces
   * struct, the purpose of this selection is to reduce the amount of data
   * that must be exchanged over the MPI network (or via `memcpy` if within
   * the shared memory region of a node) to gain performance. Note that there
   * is no way to communicate this setting with the FEFaceEvaluation class,
   * therefore this selection must be made at this site in addition to what is
   * implemented inside the `face_operation` function. As a consequence, there
   * is also no way to check that the setting passed to this call is
   * consistent with what is later done by `FEFaceEvaluation`, and it is the
   * user's responsibility to ensure correctness of data.
   *
   * @param src_vector_face_access Set the type of access into the vector
   * `src` that will happen inside the body of the @p face_operation function,
   * in analogy to `dst_vector_face_access`.
   */
  template <typename CLASS, typename OutVector, typename InVector>
  void loop (void (CLASS::*cell_operation)(const MatrixFree &,
                                           OutVector &,
                                           const InVector &,
                                           const std::pair<unsigned int,
                                           unsigned int> &)const,
             void (CLASS::*face_operation)(const MatrixFree &,
                                           OutVector &,
                                           const InVector &,
                                           const std::pair<unsigned int,
                                           unsigned int> &)const,
             void (CLASS::*boundary_operation)(const MatrixFree &,
                                               OutVector &,
                                               const InVector &,
                                               const std::pair<unsigned int,
                                               unsigned int> &)const,
             const CLASS    *owning_class,
             OutVector      &dst,
             const InVector &src,
             const bool      zero_dst_vector = false,
             const DataAccessOnFaces dst_vector_face_access = DataAccessOnFaces::unspecified,
             const DataAccessOnFaces src_vector_face_access = DataAccessOnFaces::unspecified) const;

  /**
   * Same as above, but for class member functions which are non-const.
   */
  template <typename CLASS, typename OutVector, typename InVector>
  void loop (void (CLASS::*cell_operation)(const MatrixFree &,
                                           OutVector &,
                                           const InVector &,
                                           const std::pair<unsigned int,
                                           unsigned int> &),
             void (CLASS::*face_operation)(const MatrixFree &,
                                           OutVector &,
                                           const InVector &,
                                           const std::pair<unsigned int,
                                           unsigned int> &),
             void (CLASS::*boundary_operation)(const MatrixFree &,
                                               OutVector &,
                                               const InVector &,
                                               const std::pair<unsigned int,
                                               unsigned int> &),
             CLASS          *owning_class,
             OutVector      &dst,
             const InVector &src,
             const bool      zero_dst_vector = false,
             const DataAccessOnFaces dst_vector_face_access = DataAccessOnFaces::unspecified,
             const DataAccessOnFaces src_vector_face_access = DataAccessOnFaces::unspecified) const;

  /**
   * In the hp adaptive case, a subrange of cells as computed during the cell
   * loop might contain elements of different degrees. Use this function to
   * compute what the subrange for an individual finite element degree is. The
   * finite element degree is associated to the vector component given in the
   * function call.
   */
  std::pair<unsigned int,unsigned int>
  create_cell_subrange_hp (const std::pair<unsigned int,unsigned int> &range,
                           const unsigned int fe_degree,
                           const unsigned int dof_handler_index = 0) const;

  /**
   * In the hp adaptive case, a subrange of cells as computed during the cell
   * loop might contain elements of different degrees. Use this function to
   * compute what the subrange for a given index the hp finite element, as
   * opposed to the finite element degree in the other function.
   */
  std::pair<unsigned int,unsigned int>
  create_cell_subrange_hp_by_index (const std::pair<unsigned int,unsigned int> &range,
                                    const unsigned int fe_index,
                                    const unsigned int dof_handler_index = 0) const;

  //@}

  /**
   * @name 3: Initialization of vectors
   */
  //@{
  /**
   * Initialize function for a general vector. The length of the vector is
   * equal to the total number of degrees in the DoFHandler. If the vector is
   * of class LinearAlgebra::distributed::Vector@<Number@>, the ghost entries are
   * set accordingly. For vector-valued problems with several DoFHandlers
   * underlying this class, the parameter @p vector_component defines which
   * component is to be used.
   *
   * For the vectors used with MatrixFree and in FEEvaluation, a vector needs
   * to hold all
   * @ref GlossLocallyActiveDof "locally active DoFs"
   * and also some of the
   * @ref GlossLocallyRelevantDof "locally relevant DoFs".
   * The selection of DoFs is such that one can read all degrees of freedom on all
   * locally relevant elements (locally active) plus the degrees of freedom
   * that constraints expand into from the locally owned cells. However, not
   * all locally relevant DoFs are stored because most of them would never be
   * accessed in matrix-vector products and result in too much data sent
   * around which impacts the performance.
   */
  template <typename VectorType>
  void initialize_dof_vector(VectorType &vec,
                             const unsigned int dof_handler_index=0) const;

  /**
   * Initialize function for a distributed vector. The length of the vector is
   * equal to the total number of degrees in the DoFHandler. If the vector is
   * of class LinearAlgebra::distributed::Vector@<Number@>, the ghost entries are
   * set accordingly. For vector-valued problems with several DoFHandlers
   * underlying this class, the parameter @p vector_component defines which
   * component is to be used.
   *
   * For the vectors used with MatrixFree and in FEEvaluation, a vector needs
   * to hold all
   * @ref GlossLocallyActiveDof "locally active DoFs"
   * and also some of the
   * @ref GlossLocallyRelevantDof "locally relevant DoFs".
   * The selection of DoFs is such that one can read all degrees of freedom on all
   * locally relevant elements (locally active) plus the degrees of freedom
   * that constraints expand into from the locally owned cells. However, not
   * all locally relevant DoFs are stored because most of them would never be
   * accessed in matrix-vector products and result in too much data sent
   * around which impacts the performance.
   */
  template <typename Number2>
  void initialize_dof_vector(LinearAlgebra::distributed::Vector<Number2> &vec,
                             const unsigned int dof_handler_index=0) const;

  /**
   * Return the partitioner that represents the locally owned data and the
   * ghost indices where access is needed to for the cell loop. The
   * partitioner is constructed from the locally owned dofs and ghost dofs
   * given by the respective fields. If you want to have specific information
   * about these objects, you can query them with the respective access
   * functions. If you just want to initialize a (parallel) vector, you should
   * usually prefer this data structure as the data exchange information can
   * be reused from one vector to another.
   */
  const std::shared_ptr<const Utilities::MPI::Partitioner> &
  get_vector_partitioner (const unsigned int dof_handler_index=0) const;

  /**
   * Return the set of cells that are oned by the processor.
   */
  const IndexSet &
  get_locally_owned_set (const unsigned int dof_handler_index=0) const;

  /**
   * Return the set of ghost cells needed but not owned by the processor.
   */
  const IndexSet &
  get_ghost_set (const unsigned int dof_handler_index=0) const;

  /**
   * Return a list of all degrees of freedom that are constrained. The list
   * is returned in MPI-local index space for the locally owned range of the
   * vector, not in global MPI index space that spans all MPI processors. To
   * get numbers in global index space, call
   * <tt>get_vector_partitioner()->local_to_global</tt> on an entry of the
   * vector. In addition, it only returns the indices for degrees of freedom
   * that are owned locally, not for ghosts.
   */
  const std::vector<unsigned int> &
  get_constrained_dofs (const unsigned int dof_handler_index=0) const;

  /**
   * Computes a renumbering of degrees of freedom that better fits with the
   * data layout in MatrixFree according to the given layout of data. Note that
   * this function does not re-arrange the information stored in this class,
   * but rather creates a renumbering for consumption of
   * DoFHandler::renumber_dofs. To have any effect a MatrixFree object must be
   * set up again using the renumbered DoFHandler and ConstraintMatrix. Note
   * that if a DoFHandler calls DoFHandler::renumber_dofs, all information in
   * MatrixFree becomes invalid.
   */
  void renumber_dofs (std::vector<types::global_dof_index> &renumbering,
                      const unsigned int dof_handler_index=0);

  //@}

  /**
   * @name 4: General information
   */
  //@{
  /**
   * Return whether a given FiniteElement @p fe is supported by this class.
   */
  template <int spacedim>
  static
  bool is_supported (const FiniteElement<dim, spacedim> &fe);

  /**
   * Return the number of different DoFHandlers specified at initialization.
   */
  unsigned int n_components () const;

  /**
   * For the finite element underlying the DoFHandler specified by @p
   * dof_handler_index, return the number of base elements.
   */
  unsigned int n_base_elements (const unsigned int dof_handler_index) const;

  /**
   * Return the number of cells this structure is based on. If you are using a
   * usual DoFHandler, it corresponds to the number of (locally owned) active
   * cells. Note that most data structures in this class do not directly act
   * on this number but rather on n_cell_batches() which gives the number of
   * cells as seen when lumping several cells together with vectorization.
   */
  unsigned int n_physical_cells () const;

  /**
   * Return the number of cell batches that this structure works on.  The
   * batches are formed by application of vectorization over several cells in
   * general. The cell range in @p cell_loop runs from zero to n_cell_batches()
   * (exclusive), so this is the appropriate size if you want to store arrays
   * of data for all cells to be worked on. This number is approximately
   * n_physical_cells()/VectorizedArray::n_array_elements (depending on how
   * many cell chunks that do not get filled up completely).
   */
  unsigned int n_macro_cells () const;

  /**
   * Return the number of cell batches that this structure works on. The
   * batches are formed by application of vectorization over several cells in
   * general. The cell range in @p cell_loop runs from zero to
   * n_cell_batches() (exclusive), so this is the appropriate size if you want
   * to store arrays of data for all cells to be worked on. This number is
   * approximately n_physical_cells()/VectorizedArray::n_array_elements
   * (depending on how many cell chunks that do not get filled up completely).
   */
  unsigned int n_cell_batches () const;

  /**
   * Returns the number of additional cell batches that this structure keeps
   * for face integration. Note that not all cells that are ghosted in the
   * triangulation are kept in this data structure, but only the ones which
   * are necessary for evaluating face integrals from both sides.
   */
  unsigned int n_ghost_cell_batches () const;

  /**
   * Returns the number of interior face batches that this structure works on.
   * The batches are formed by application of vectorization over several faces
   * in general. The face range in @p loop runs from zero to
   * n_inner_face_batches() (exclusive), so this is the appropriate size if
   * you want to store arrays of data for all interior faces to be worked on.
   */
  unsigned int n_inner_face_batches () const;

  /**
   * Returns the number of boundary face batches that this structure works on.
   * The batches are formed by application of vectorization over several faces
   * in general. The face range in @p loop runs from n_inner_face_batches() to
   * n_inner_face_batches()+n_boundary_face_batches() (exclusive), so if you
   * need to store arrays that hold data for all boundary faces but not the
   * interior ones, this number gives the appropriate size.
   */
  unsigned int n_boundary_face_batches () const;

  /**
   * Returns the number of faces that are not processed locally but belong to
   * locally owned faces.
   */
  unsigned int n_ghost_inner_face_batches() const;

  /**
   * In order to apply different operators to different parts of the boundary,
   * this method can be used to query the boundary id of a given face in the
   * faces' own sorting by lanes in a VectorizedArray. Only valid for an index
   * indicating a boundary face.
   */
  types::boundary_id get_boundary_id (const unsigned int macro_face) const;

  /**
   * Returns the boundary ids for the faces within a cell, using the cells'
   * sorting by lanes in the VectorizedArray.
   */
  std::array<types::boundary_id, VectorizedArray<Number>::n_array_elements>
  get_faces_by_cells_boundary_id (const unsigned int macro_cell,
                                  const unsigned int face_number) const;

  /**
   * In case this structure was built based on a DoFHandler, this returns the
   * DoFHandler.
   */
  const DoFHandler<dim> &
  get_dof_handler (const unsigned int dof_handler_index = 0) const;

  /**
   * This returns the cell iterator in deal.II speak to a given cell in the
   * renumbering of this structure.
   *
   * Note that the cell iterators in deal.II go through cells differently to
   * what the cell loop of this class does. This is because several cells are
   * worked on together (vectorization), and since cells with neighbors on
   * different MPI processors need to be accessed at a certain time when
   * accessing remote data and overlapping communication with computation.
   */
  typename DoFHandler<dim>::cell_iterator
  get_cell_iterator (const unsigned int macro_cell_number,
                     const unsigned int vector_number,
                     const unsigned int fe_component = 0) const;

  /**
   * This returns the cell iterator in deal.II speak to a given cell in the
   * renumbering of this structure. This function returns an exception in case
   * the structure was not constructed based on an hp::DoFHandler.
   *
   * Note that the cell iterators in deal.II go through cells differently to
   * what the cell loop of this class does. This is because several cells are
   * worked on together (vectorization), and since cells with neighbors on
   * different MPI processors need to be accessed at a certain time when
   * accessing remote data and overlapping communication with computation.
   */
  typename hp::DoFHandler<dim>::active_cell_iterator
  get_hp_cell_iterator (const unsigned int macro_cell_number,
                        const unsigned int vector_number,
                        const unsigned int dof_handler_index = 0) const;

  /**
   * Since this class uses vectorized data types with usually more than one
   * value in the data field, a situation might occur when some components of
   * the vector type do not correspond to an actual cell in the mesh. When
   * using only this class, one usually does not need to bother about that
   * fact since the values are padded with zeros. However, when this class is
   * mixed with deal.II access to cells, care needs to be taken. This function
   * returns @p true if not all @p vectorization_length cells for the given @p
   * macro_cell are real cells. To find out how many cells are actually used,
   * use the function @p n_active_entries_per_cell_batch.
   */
  bool
  at_irregular_cell (const unsigned int macro_cell_number) const;

  /**
   * This query returns how many cells over the length of vectorization data
   * types correspond to actual cells in the mesh. For most given @p
   * cell_batch_number, this is just @p vectorization_length many, but there
   * might be one or a few meshes (where the numbers do not add up) where
   * there are less such components filled, indicated by the function @p
   * at_irregular_cell.
   */
  unsigned int
  n_components_filled (const unsigned int cell_batch_number) const;

  /**
   * This query returns how many cells over the length of vectorization data
   * types correspond to actual cells in the mesh. For most given cell batches
   * in n_cell_batches(), this is just @p vectorization_length many, but there
   * might be one or a few meshes (where the numbers do not add up) where
   * there are less such components filled, indicated by the function @p
   * at_irregular_cell.
   */
  unsigned int
  n_active_entries_per_cell_batch (const unsigned int cell_batch_number) const;

  /**
   * Use this function to find out how many faces over the length of
   * vectorization data types correspond to real faces (both interior and
   * boundary faces, as those use the same indexing but with different ranges)
   * in the mesh. For most given indices in n_inner_faces_batches() and
   * n_boundary_face_batches(), this is just @p vectorization_length many, but
   * there might be one or a few meshes (where the numbers do not add up)
   * where there are less such lanes filled.
   */
  unsigned int
  n_active_entries_per_face_batch (const unsigned int face_batch_number) const;

  /**
   * Return the number of degrees of freedom per cell for a given hp index.
   */
  unsigned int
  get_dofs_per_cell (const unsigned int dof_handler_index = 0,
                     const unsigned int hp_active_fe_index = 0) const;

  /**
   * Return the number of quadrature points per cell for a given hp index.
   */
  unsigned int
  get_n_q_points (const unsigned int quad_index = 0,
                  const unsigned int hp_active_fe_index = 0) const;

  /**
   * Return the number of degrees of freedom on each face of the cell for
   * given hp index.
   */
  unsigned int
  get_dofs_per_face (const unsigned int fe_component = 0,
                     const unsigned int hp_active_fe_index = 0) const;

  /**
   * Return the number of quadrature points on each face of the cell for
   * given hp index.
   */
  unsigned int
  get_n_q_points_face (const unsigned int quad_index = 0,
                       const unsigned int hp_active_fe_index = 0) const;

  /**
   * Return the quadrature rule for given hp index.
   */
  const Quadrature<dim> &
  get_quadrature (const unsigned int quad_index = 0,
                  const unsigned int hp_active_fe_index = 0) const;

  /**
   * Return the quadrature rule for given hp index.
   */
  const Quadrature<dim-1> &
  get_face_quadrature (const unsigned int quad_index = 0,
                       const unsigned int hp_active_fe_index = 0) const;

  /**
   * Return the category the current batch of cells was assigned to. Categories
   * run between the given values in the field
   * AdditionalData::cell_vectorization_category for non-hp DoFHandler types
   * and return the active FE index in the hp-adaptive case.
   */
  unsigned int get_cell_category (const unsigned int macro_cell) const;

  /**
   * Return the category on the cells on the two sides of the current batch of
   * faces.
   */
  std::pair<unsigned int,unsigned int>
  get_face_category (const unsigned int macro_face) const;

  /**
   * Queries whether or not the indexation has been set.
   */
  bool indices_initialized () const;

  /**
   * Queries whether or not the geometry-related information for the cells has
   * been set.
   */

  bool mapping_initialized () const;

  /**
   * Return an approximation of the memory consumption of this class in
   * bytes.
   */
  std::size_t memory_consumption() const;

  /**
   * Prints a detailed summary of memory consumption in the different
   * structures of this class to the given output stream.
   */
  template <typename StreamType>
  void print_memory_consumption(StreamType &out) const;

  /**
   * Prints a summary of this class to the given output stream. It is focused
   * on the indices, and does not print all the data stored.
   */
  void print (std::ostream &out) const;

  //@}

  /**
   * @name 5: Access of internal data structure (expert mode, interface not
   * stable between releases)
   */
  //@{
  /**
   * Return information on task graph.
   */
  const internal::MatrixFreeFunctions::TaskInfo &
  get_task_info () const;

  /**
   * Return information on system size.
   */
  DEAL_II_DEPRECATED
  const internal::MatrixFreeFunctions::TaskInfo &
  get_size_info () const;

  /*
   * Return geometry-dependent information on the cells.
   */
  const internal::MatrixFreeFunctions::MappingInfo<dim,Number> &
  get_mapping_info () const;

  /**
   * Return information on indexation degrees of freedom.
   */
  const internal::MatrixFreeFunctions::DoFInfo &
  get_dof_info (const unsigned int dof_handler_index_component = 0) const;

  /**
   * Return the number of weights in the constraint pool.
   */
  unsigned int n_constraint_pool_entries() const;

  /**
   * Return a pointer to the first number in the constraint pool data with
   * index @p pool_index (to be used together with @p constraint_pool_end()).
   */
  const Number *
  constraint_pool_begin (const unsigned int pool_index) const;

  /**
   * Return a pointer to one past the last number in the constraint pool data
   * with index @p pool_index (to be used together with @p
   * constraint_pool_begin()).
   */
  const Number *
  constraint_pool_end (const unsigned int pool_index) const;

  /**
   * Return the unit cell information for given hp index.
   */
  const internal::MatrixFreeFunctions::ShapeInfo<VectorizedArray<Number> > &
  get_shape_info (const unsigned int dof_handler_index_component = 0,
                  const unsigned int quad_index                  = 0,
                  const unsigned int fe_base_element             = 0,
                  const unsigned int hp_active_fe_index          = 0,
                  const unsigned int hp_active_quad_index        = 0) const;

  /**
   * Return the connectivity information of a face.
   */
  const internal::MatrixFreeFunctions::FaceToCellTopology<VectorizedArray<Number>::n_array_elements> &
  get_face_info (const unsigned int face_batch_number) const;

  /**
   * Obtains a scratch data object for internal use. Make sure to release it
   * afterwards by passing the pointer you obtain from this object to the
   * release_scratch_data() function. This interface is used by FEEvaluation
   * objects for storing their data structures.
   *
   * The organization of the internal data structure is a thread-local storage
   * of a list of vectors. Multiple threads will each get a separate storage
   * field and separate vectors, ensuring thread safety. The mechanism to
   * acquire and release objects is similar to the mechanisms used for the
   * local contributions of WorkStream, see
   * @ref workstream_paper "the WorkStream paper".
   */
  AlignedVector<VectorizedArray<Number> > *acquire_scratch_data() const;

  /**
   * Makes the object of the scratchpad available again.
   */
  void release_scratch_data(const AlignedVector<VectorizedArray<Number> > *memory) const;

  /**
   * Obtains a scratch data object for internal use. Make sure to release it
   * afterwards by passing the pointer you obtain from this object to the
   * release_scratch_data_non_threadsafe() function. Note that, as opposed to
   * acquire_scratch_data(), this method can only be called by a single thread
   * at a time, but opposed to the acquire_scratch_data() it is also possible
   * that the thread releasing the scratch data can be different than the one
   * that acquired it.
   */
  AlignedVector<Number> *acquire_scratch_data_non_threadsafe() const;

  /**
   * Makes the object of the scratch data available again.
   */
  void release_scratch_data_non_threadsafe(const AlignedVector<Number> *memory) const;

  //@}

private:

  /**
   * This is the actual reinit function that sets up the indices for the
   * DoFHandler case.
   */
  void internal_reinit (const Mapping<dim>                &mapping,
                        const std::vector<const DoFHandler<dim> *> &dof_handler,
                        const std::vector<const ConstraintMatrix *> &constraint,
                        const std::vector<IndexSet>       &locally_owned_set,
                        const std::vector<hp::QCollection<1> > &quad,
                        const AdditionalData              &additional_data);

  /**
   * Same as before but for hp::DoFHandler instead of generic DoFHandler type.
   */
  void internal_reinit (const Mapping<dim>               &mapping,
                        const std::vector<const hp::DoFHandler<dim>*> &dof_handler,
                        const std::vector<const ConstraintMatrix *> &constraint,
                        const std::vector<IndexSet>      &locally_owned_set,
                        const std::vector<hp::QCollection<1> > &quad,
                        const AdditionalData             &additional_data);

  /**
   * Initializes the fields in DoFInfo together with the constraint pool that
   * holds all different weights in the constraints (not part of DoFInfo
   * because several DoFInfo classes can have the same weights which
   * consequently only need to be stored once).
   */
  void
  initialize_indices (const std::vector<const ConstraintMatrix *> &constraint,
                      const std::vector<IndexSet> &locally_owned_set,
                      const AdditionalData        &additional_data);

  /**
   * Initializes the DoFHandlers based on a DoFHandler<dim> argument.
   */
  void initialize_dof_handlers (const std::vector<const DoFHandler<dim>*> &dof_handlers,
                                const AdditionalData &additional_data);

  /**
   * Initializes the DoFHandlers based on a hp::DoFHandler<dim> argument.
   */
  void initialize_dof_handlers (const std::vector<const hp::DoFHandler<dim>*> &dof_handlers,
                                const AdditionalData &additional_data);

  /**
   * Setup connectivity graph with information on the dependencies between
   * block due to shared faces.
   */
  void make_connectivity_graph_faces (DynamicSparsityPattern &connectivity);

  /**
   * This struct defines which DoFHandler has actually been given at
   * construction, in order to define the correct behavior when querying the
   * underlying DoFHandler.
   */
  struct DoFHandlers
  {
    DoFHandlers ()
      :
      active_dof_handler(usual),
      n_dof_handlers (0),
      level (numbers::invalid_unsigned_int)
    {}

    std::vector<SmartPointer<const DoFHandler<dim> > >   dof_handler;
    std::vector<SmartPointer<const hp::DoFHandler<dim> > > hp_dof_handler;
    enum ActiveDoFHandler
    {
      /**
       * Use DoFHandler.
       */
      usual,
      /**
       * Use hp::DoFHandler.
       */
      hp
    } active_dof_handler;
    unsigned int n_dof_handlers;
    unsigned int level;
  };

  /**
   * Pointers to the DoFHandlers underlying the current problem.
   */
  DoFHandlers dof_handlers;

  /**
   * Contains the information about degrees of freedom on the individual cells
   * and constraints.
   */
  std::vector<internal::MatrixFreeFunctions::DoFInfo> dof_info;

  /**
   * Contains the weights for constraints stored in DoFInfo. Filled into a
   * separate field since several vector components might share similar
   * weights, which reduces memory consumption. Moreover, it obviates template
   * arguments on DoFInfo and keeps it a plain field of indices only.
   */
  std::vector<Number> constraint_pool_data;

  /**
   * Contains an indicator to the start of the ith index in the constraint
   * pool data.
   */
  std::vector<unsigned int> constraint_pool_row_index;

  /**
   * Holds information on transformation of cells from reference cell to real
   * cell that is needed for evaluating integrals.
   */
  internal::MatrixFreeFunctions::MappingInfo<dim,Number> mapping_info;

  /**
   * Contains shape value information on the unit cell.
   */
  Table<4,internal::MatrixFreeFunctions::ShapeInfo<VectorizedArray<Number>>> shape_info;

  /**
   * Describes how the cells are gone through. With the cell level (first
   * index in this field) and the index within the level, one can reconstruct
   * a deal.II cell iterator and use all the traditional things deal.II offers
   * to do with cell iterators.
   */
  std::vector<std::pair<unsigned int,unsigned int> > cell_level_index;


  /**
   * For discontinuous Galerkin, the cell_level_index includes cells that are
   * not on the local processor but that are needed to evaluate the cell
   * integrals. In cell_level_index_end_local, we store the number of local
   * cells.
   */
  unsigned int cell_level_index_end_local;

  /**
   * Stores the basic layout of the cells and faces to be treated, including
   * the task layout for the shared memory parallelization and possible
   * overlaps between communications and computations with MPI.
   */
  internal::MatrixFreeFunctions::TaskInfo task_info;

  /**
   * Vector holding face information. Only initialized if
   * build_face_info=true.
   */
  internal::MatrixFreeFunctions::FaceInfo<VectorizedArray<Number>::n_array_elements> face_info;

  /**
   * Stores whether indices have been initialized.
   */
  bool                               indices_are_initialized;

  /**
   * Stores whether indices have been initialized.
   */
  bool                               mapping_is_initialized;

  /**
   * Scratchpad memory for use in evaluation. We allow more than one
   * evaluation object to attach to this field (this, the outer
   * std::vector), so we need to keep tracked of whether a certain data
   * field is already used (first part of pair) and keep a list of
   * objects.
   */
  mutable Threads::ThreadLocalStorage<std::list<std::pair<bool, AlignedVector<VectorizedArray<Number> > > > > scratch_pad;

  /**
   * Scratchpad memory for use in evaluation and other contexts, non-thread
   * safe variant.
   */
  mutable std::list<std::pair<bool, AlignedVector<Number> > > scratch_pad_non_threadsafe;
};



/*----------------------- Inline functions ----------------------------------*/

#ifndef DOXYGEN



template <int dim, typename Number>
template <typename VectorType>
inline
void
MatrixFree<dim,Number>::initialize_dof_vector(VectorType &vec,
                                              const unsigned int comp) const
{
  AssertIndexRange(comp, n_components());
  vec.reinit(dof_info[comp].vector_partitioner->size());
}



template <int dim, typename Number>
template <typename Number2>
inline
void
MatrixFree<dim,Number>::initialize_dof_vector(LinearAlgebra::distributed::Vector<Number2> &vec,
                                              const unsigned int comp) const
{
  AssertIndexRange(comp, n_components());
  vec.reinit(dof_info[comp].vector_partitioner);
}



template <int dim, typename Number>
inline
const std::shared_ptr<const Utilities::MPI::Partitioner> &
MatrixFree<dim,Number>::get_vector_partitioner (const unsigned int comp) const
{
  AssertIndexRange(comp, n_components());
  return dof_info[comp].vector_partitioner;
}



template <int dim, typename Number>
inline
const std::vector<unsigned int> &
MatrixFree<dim,Number>::get_constrained_dofs (const unsigned int comp) const
{
  AssertIndexRange(comp, n_components());
  return dof_info[comp].constrained_dofs;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_components () const
{
  AssertDimension (dof_handlers.n_dof_handlers, dof_info.size());
  return dof_handlers.n_dof_handlers;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_base_elements (const unsigned int dof_no) const
{
  AssertDimension (dof_handlers.n_dof_handlers, dof_info.size());
  AssertIndexRange(dof_no, dof_handlers.n_dof_handlers);
  return dof_handlers.dof_handler[dof_no]->get_fe().n_base_elements();
}



template <int dim, typename Number>
inline
const internal::MatrixFreeFunctions::TaskInfo &
MatrixFree<dim,Number>::get_task_info () const
{
  return task_info;
}



template <int dim, typename Number>
inline
const internal::MatrixFreeFunctions::TaskInfo &
MatrixFree<dim,Number>::get_size_info () const
{
  return task_info;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_macro_cells () const
{
  return *(task_info.cell_partition_data.end()-2);
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_physical_cells () const
{
  return task_info.n_active_cells;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_cell_batches () const
{
  return *(task_info.cell_partition_data.end()-2);
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_ghost_cell_batches () const
{
  return *(task_info.cell_partition_data.end()-1)-
         *(task_info.cell_partition_data.end()-2);
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_inner_face_batches () const
{
  if (task_info.face_partition_data.size() == 0)
    return 0;
  return task_info.face_partition_data.back();
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_boundary_face_batches () const
{
  if (task_info.face_partition_data.size() == 0)
    return 0;
  return task_info.boundary_partition_data.back()-task_info.face_partition_data.back();
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_ghost_inner_face_batches () const
{
  if (task_info.face_partition_data.size() == 0)
    return 0;
  return face_info.faces.size() - task_info.boundary_partition_data.back();
}



template <int dim, typename Number>
inline
types::boundary_id
MatrixFree<dim,Number>::get_boundary_id(const unsigned int macro_face) const
{
  Assert(macro_face >= task_info.boundary_partition_data[0] &&
         macro_face < task_info.boundary_partition_data.back(),
         ExcIndexRange(macro_face,
                       task_info.boundary_partition_data[0],
                       task_info.boundary_partition_data.back()));
  return types::boundary_id(face_info.faces[macro_face].exterior_face_no);
}



template <int dim, typename Number>
inline
std::array<types::boundary_id, VectorizedArray<Number>::n_array_elements>
MatrixFree<dim,Number>::get_faces_by_cells_boundary_id (const unsigned int macro_cell,
                                                        const unsigned int face_number) const
{
  AssertIndexRange(macro_cell, n_macro_cells());
  AssertIndexRange(face_number, GeometryInfo<dim>::faces_per_cell);
  Assert(face_info.cell_and_face_boundary_id.size(0)>=n_macro_cells(),
         ExcNotInitialized());
  std::array<types::boundary_id, VectorizedArray<Number>::n_array_elements> result;
  result.fill(numbers::invalid_boundary_id);
  for (unsigned int v=0; v<n_active_entries_per_cell_batch(macro_cell); ++v)
    result[v] = face_info.cell_and_face_boundary_id(macro_cell, face_number, v);
  return result;
}



template <int dim, typename Number>
inline
const internal::MatrixFreeFunctions::MappingInfo<dim,Number> &
MatrixFree<dim,Number>::get_mapping_info () const
{
  return mapping_info;
}



template <int dim, typename Number>
inline
const internal::MatrixFreeFunctions::DoFInfo &
MatrixFree<dim,Number>::get_dof_info (const unsigned int dof_index) const
{
  AssertIndexRange (dof_index, n_components());
  return dof_info[dof_index];
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_constraint_pool_entries() const
{
  return constraint_pool_row_index.size()-1;
}



template <int dim, typename Number>
inline
const Number *
MatrixFree<dim,Number>::constraint_pool_begin (const unsigned int row) const
{
  AssertIndexRange (row, constraint_pool_row_index.size()-1);
  return constraint_pool_data.empty() ? nullptr :
         constraint_pool_data.data() + constraint_pool_row_index[row];
}



template <int dim, typename Number>
inline
const Number *
MatrixFree<dim,Number>::constraint_pool_end (const unsigned int row) const
{
  AssertIndexRange (row, constraint_pool_row_index.size()-1);
  return constraint_pool_data.empty() ? nullptr :
         constraint_pool_data.data() + constraint_pool_row_index[row+1];
}



template <int dim, typename Number>
inline
std::pair<unsigned int,unsigned int>
MatrixFree<dim,Number>::create_cell_subrange_hp
(const std::pair<unsigned int,unsigned int> &range,
 const unsigned int degree,
 const unsigned int dof_handler_component) const
{
  if (dof_info[dof_handler_component].cell_active_fe_index.empty())
    {
      AssertDimension (dof_info[dof_handler_component].fe_index_conversion.size(),1);
      AssertDimension (dof_info[dof_handler_component].fe_index_conversion[0].size(), 1);
      if (dof_info[dof_handler_component].fe_index_conversion[0][0] == degree)
        return range;
      else
        return std::pair<unsigned int,unsigned int> (range.second,range.second);
    }

  const unsigned int fe_index =
    dof_info[dof_handler_component].fe_index_from_degree(0, degree);
  if (fe_index >= dof_info[dof_handler_component].max_fe_index)
    return std::pair<unsigned int,unsigned int>(range.second, range.second);
  else
    return create_cell_subrange_hp_by_index (range, fe_index, dof_handler_component);
}



template <int dim, typename Number>
inline
bool
MatrixFree<dim,Number>::at_irregular_cell (const unsigned int macro_cell) const
{
  AssertIndexRange (macro_cell, task_info.cell_partition_data.back());
  return VectorizedArray<Number>::n_array_elements > 1 &&
         cell_level_index[(macro_cell+1)*VectorizedArray<Number>::n_array_elements-1] ==
         cell_level_index[(macro_cell+1)*VectorizedArray<Number>::n_array_elements-2];
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_components_filled (const unsigned int cell_batch_number) const
{
  return n_active_entries_per_cell_batch(cell_batch_number);
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_active_entries_per_cell_batch(const unsigned int cell_batch_number) const
{
  AssertIndexRange (cell_batch_number, task_info.cell_partition_data.back());
  unsigned int n_components = VectorizedArray<Number>::n_array_elements;
  while (n_components > 1 &&
         cell_level_index[cell_batch_number*VectorizedArray<Number>::n_array_elements+n_components-1] ==
         cell_level_index[cell_batch_number*VectorizedArray<Number>::n_array_elements+n_components-2])
    --n_components;
  AssertIndexRange(n_components-1, VectorizedArray<Number>::n_array_elements);
  return n_components;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::n_active_entries_per_face_batch(const unsigned int face_batch_number) const
{
  AssertIndexRange (face_batch_number, face_info.faces.size());
  unsigned int n_components = VectorizedArray<Number>::n_array_elements;
  while (n_components > 1 &&
         face_info.faces[face_batch_number].cells_interior[n_components-1] ==
         numbers::invalid_unsigned_int)
    --n_components;
  AssertIndexRange(n_components-1, VectorizedArray<Number>::n_array_elements);
  return n_components;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::get_dofs_per_cell(const unsigned int dof_handler_index,
                                          const unsigned int active_fe_index) const
{
  return dof_info[dof_handler_index].dofs_per_cell[active_fe_index];
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::get_n_q_points(const unsigned int quad_index,
                                       const unsigned int active_fe_index) const
{
  AssertIndexRange (quad_index, mapping_info.cell_data.size());
  return mapping_info.cell_data[quad_index].descriptor[active_fe_index].n_q_points;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::get_dofs_per_face(const unsigned int dof_handler_index,
                                          const unsigned int active_fe_index) const
{
  return dof_info[dof_handler_index].dofs_per_face[active_fe_index];
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::get_n_q_points_face(const unsigned int quad_index,
                                            const unsigned int active_fe_index) const
{
  AssertIndexRange (quad_index, mapping_info.face_data.size());
  return mapping_info.face_data[quad_index].descriptor[active_fe_index].n_q_points;
}



template <int dim, typename Number>
inline
const IndexSet &
MatrixFree<dim,Number>::get_locally_owned_set(const unsigned int dof_handler_index) const
{
  return dof_info[dof_handler_index].vector_partitioner->locally_owned_range();
}



template <int dim, typename Number>
inline
const IndexSet &
MatrixFree<dim,Number>::get_ghost_set(const unsigned int dof_handler_index) const
{
  return dof_info[dof_handler_index].vector_partitioner->ghost_indices();
}



template <int dim, typename Number>
inline
const internal::MatrixFreeFunctions::ShapeInfo<VectorizedArray<Number> > &
MatrixFree<dim,Number>::get_shape_info (const unsigned int dof_handler_index,
                                        const unsigned int index_quad,
                                        const unsigned int index_fe,
                                        const unsigned int active_fe_index,
                                        const unsigned int active_quad_index) const
{
  AssertIndexRange(dof_handler_index, dof_info.size());
  const unsigned int ind = dof_info[dof_handler_index].global_base_element_offset+index_fe;
  AssertIndexRange (ind, shape_info.size(0));
  AssertIndexRange (index_quad, shape_info.size(1));
  AssertIndexRange (active_fe_index, shape_info.size(2));
  AssertIndexRange (active_quad_index, shape_info.size(3));
  return shape_info(ind, index_quad,
                    active_fe_index, active_quad_index);
}



template <int dim, typename Number>
inline
const internal::MatrixFreeFunctions::FaceToCellTopology<VectorizedArray<Number>::n_array_elements> &
MatrixFree<dim,Number>::get_face_info (const unsigned int macro_face) const
{
  AssertIndexRange(macro_face, face_info.faces.size());
  return face_info.faces[macro_face];
}



template <int dim, typename Number>
inline
const Quadrature<dim> &
MatrixFree<dim,Number>::get_quadrature (const unsigned int quad_index,
                                        const unsigned int active_fe_index) const
{
  AssertIndexRange (quad_index, mapping_info.cell_data.size());
  return mapping_info.cell_data[quad_index].descriptor[active_fe_index].quadrature;
}



template <int dim, typename Number>
inline
const Quadrature<dim-1> &
MatrixFree<dim,Number>::get_face_quadrature (const unsigned int quad_index,
                                             const unsigned int active_fe_index) const
{
  AssertIndexRange (quad_index, mapping_info.face_data.size());
  return mapping_info.face_data[quad_index].descriptor[active_fe_index].quadrature;
}



template <int dim, typename Number>
inline
unsigned int
MatrixFree<dim,Number>::get_cell_category (const unsigned int macro_cell) const
{
  AssertIndexRange(0, dof_info.size());
  AssertIndexRange(macro_cell, dof_info[0].cell_active_fe_index.size());
  if (dof_info[0].cell_active_fe_index.empty())
    return 0;
  else
    return dof_info[0].cell_active_fe_index[macro_cell];
}



template <int dim, typename Number>
inline
std::pair<unsigned int,unsigned int>
MatrixFree<dim,Number>::get_face_category (const unsigned int macro_face) const
{
  AssertIndexRange(macro_face, face_info.faces.size());
  if (dof_info[0].cell_active_fe_index.empty())
    return std::make_pair(0U, 0U);

  std::pair<unsigned int,unsigned int> result;
  for (unsigned int v=0; v<VectorizedArray<Number>::n_array_elements &&
       face_info.faces[macro_face].cells_interior[v] != numbers::invalid_unsigned_int; ++v)
    result.first = std::max(result.first,
                            dof_info[0].cell_active_fe_index[face_info.faces[macro_face].cells_interior[v]]);
  if (face_info.faces[macro_face].cells_exterior[0] != numbers::invalid_unsigned_int)
    for (unsigned int v=0; v<VectorizedArray<Number>::n_array_elements &&
         face_info.faces[macro_face].cells_exterior[v] != numbers::invalid_unsigned_int; ++v)
      result.second = std::max(result.first,
                               dof_info[0].cell_active_fe_index[face_info.faces[macro_face].cells_exterior[v]]);
  else
    result.second = numbers::invalid_unsigned_int;
  return result;
}



template <int dim, typename Number>
inline
bool
MatrixFree<dim,Number>::indices_initialized () const
{
  return indices_are_initialized;
}



template <int dim, typename Number>
inline
bool
MatrixFree<dim,Number>::mapping_initialized () const
{
  return mapping_is_initialized;
}



template <int dim,typename Number>
AlignedVector<VectorizedArray<Number> > *
MatrixFree<dim,Number>::acquire_scratch_data() const
{
  typedef std::list<std::pair<bool, AlignedVector<VectorizedArray<Number> > > > list_type;
  list_type &data = scratch_pad.get();
  for (typename list_type::iterator it=data.begin(); it!=data.end(); ++it)
    if (it->first == false)
      {
        it->first = true;
        return &it->second;
      }
  data.push_front(std::make_pair(true,AlignedVector<VectorizedArray<Number> >()));
  return &data.front().second;
}



template <int dim, typename Number>
void
MatrixFree<dim,Number>::release_scratch_data(const AlignedVector<VectorizedArray<Number> > *scratch) const
{
  typedef std::list<std::pair<bool, AlignedVector<VectorizedArray<Number> > > > list_type;
  list_type &data = scratch_pad.get();
  for (typename list_type::iterator it=data.begin(); it!=data.end(); ++it)
    if (&it->second == scratch)
      {
        Assert(it->first == true, ExcInternalError());
        it->first = false;
        return;
      }
  AssertThrow(false, ExcMessage("Tried to release invalid scratch pad"));
}



template <int dim,typename Number>
AlignedVector<Number> *
MatrixFree<dim,Number>::acquire_scratch_data_non_threadsafe() const
{
  for (typename std::list<std::pair<bool, AlignedVector<Number> > >::iterator
       it=scratch_pad_non_threadsafe.begin(); it!=scratch_pad_non_threadsafe.end(); ++it)
    if (it->first == false)
      {
        it->first = true;
        return &it->second;
      }
  scratch_pad_non_threadsafe.push_front(std::make_pair(true,AlignedVector<Number>()));
  return &scratch_pad_non_threadsafe.front().second;
}



template <int dim, typename Number>
void
MatrixFree<dim,Number>::release_scratch_data_non_threadsafe(const AlignedVector<Number> *scratch) const
{
  for (typename std::list<std::pair<bool, AlignedVector<Number> > >::iterator
       it=scratch_pad_non_threadsafe.begin(); it!=scratch_pad_non_threadsafe.end(); ++it)
    if (&it->second == scratch)
      {
        Assert(it->first == true, ExcInternalError());
        it->first = false;
        return;
      }
  AssertThrow(false, ExcMessage("Tried to release invalid scratch pad"));
}



// ------------------------------ reinit functions ---------------------------

namespace internal
{
  namespace MatrixFreeImplementation
  {
    template <typename DoFHandlerType>
    inline
    std::vector<IndexSet>
    extract_locally_owned_index_sets (const std::vector<const DoFHandlerType *> &dofh,
                                      const unsigned int level)
    {
      std::vector<IndexSet> locally_owned_set;
      locally_owned_set.reserve (dofh.size());
      for (unsigned int j=0; j<dofh.size(); j++)
        if (level == numbers::invalid_unsigned_int)
          locally_owned_set.push_back(dofh[j]->locally_owned_dofs());
        else
          AssertThrow(false, ExcNotImplemented());
      return locally_owned_set;
    }

    template <int dim, int spacedim>
    inline
    std::vector<IndexSet>
    extract_locally_owned_index_sets (const std::vector<const ::dealii::DoFHandler<dim,spacedim> *> &dofh,
                                      const unsigned int level)
    {
      std::vector<IndexSet> locally_owned_set;
      locally_owned_set.reserve (dofh.size());
      for (unsigned int j=0; j<dofh.size(); j++)
        if (level == numbers::invalid_unsigned_int)
          locally_owned_set.push_back(dofh[j]->locally_owned_dofs());
        else
          locally_owned_set.push_back(dofh[j]->locally_owned_mg_dofs(level));
      return locally_owned_set;
    }
  }
}



template <int dim, typename Number>
template <typename DoFHandlerType, typename QuadratureType>
void MatrixFree<dim,Number>::
reinit(const DoFHandlerType                                  &dof_handler,
       const ConstraintMatrix                                &constraints_in,
       const QuadratureType                                  &quad,
       const typename MatrixFree<dim,Number>::AdditionalData additional_data)
{
  std::vector<const DoFHandlerType *>   dof_handlers;
  std::vector<const ConstraintMatrix *> constraints;
  std::vector<QuadratureType>           quads;

  dof_handlers.push_back(&dof_handler);
  constraints.push_back (&constraints_in);
  quads.push_back (quad);

  std::vector<IndexSet> locally_owned_sets =
    internal::MatrixFreeImplementation::extract_locally_owned_index_sets
    (dof_handlers, additional_data.level_mg_handler);

  std::vector<hp::QCollection<1> > quad_hp;
  quad_hp.emplace_back (quad);

  internal_reinit(StaticMappingQ1<dim>::mapping, dof_handlers,constraints,
                  locally_owned_sets, quad_hp, additional_data);
}



template <int dim, typename Number>
template <typename DoFHandlerType, typename QuadratureType>
void MatrixFree<dim,Number>::
reinit(const Mapping<dim>                                    &mapping,
       const DoFHandlerType                                  &dof_handler,
       const ConstraintMatrix                                &constraints_in,
       const QuadratureType                                  &quad,
       const typename MatrixFree<dim,Number>::AdditionalData additional_data)
{
  std::vector<const DoFHandlerType *>   dof_handlers;
  std::vector<const ConstraintMatrix *> constraints;

  dof_handlers.push_back(&dof_handler);
  constraints.push_back (&constraints_in);

  std::vector<IndexSet> locally_owned_sets =
    internal::MatrixFreeImplementation::extract_locally_owned_index_sets
    (dof_handlers, additional_data.level_mg_handler);

  std::vector<hp::QCollection<1> > quad_hp;
  quad_hp.emplace_back (quad);

  internal_reinit(mapping, dof_handlers,constraints,locally_owned_sets,
                  quad_hp,  additional_data);
}



template <int dim, typename Number>
template <typename DoFHandlerType, typename QuadratureType>
void MatrixFree<dim,Number>::
reinit(const std::vector<const DoFHandlerType *>   &dof_handler,
       const std::vector<const ConstraintMatrix *> &constraint,
       const std::vector<QuadratureType>           &quad,
       const typename MatrixFree<dim,Number>::AdditionalData additional_data)
{
  std::vector<IndexSet> locally_owned_set =
    internal::MatrixFreeImplementation::extract_locally_owned_index_sets
    (dof_handler, additional_data.level_mg_handler);
  std::vector<hp::QCollection<1> > quad_hp;
  for (unsigned int q=0; q<quad.size(); ++q)
    quad_hp.emplace_back (quad[q]);
  internal_reinit(StaticMappingQ1<dim>::mapping, dof_handler,constraint,
                  locally_owned_set, quad_hp, additional_data);
}



template <int dim, typename Number>
template <typename DoFHandlerType, typename QuadratureType>
void MatrixFree<dim,Number>::
reinit(const std::vector<const DoFHandlerType *>             &dof_handler,
       const std::vector<const ConstraintMatrix *>           &constraint,
       const QuadratureType                                  &quad,
       const typename MatrixFree<dim,Number>::AdditionalData additional_data)
{
  std::vector<IndexSet> locally_owned_set =
    internal::MatrixFreeImplementation::extract_locally_owned_index_sets
    (dof_handler, additional_data.level_mg_handler);
  std::vector<hp::QCollection<1> > quad_hp;
  quad_hp.emplace_back (quad);
  internal_reinit(StaticMappingQ1<dim>::mapping, dof_handler,constraint,
                  locally_owned_set, quad_hp, additional_data);
}



template <int dim, typename Number>
template <typename DoFHandlerType, typename QuadratureType>
void MatrixFree<dim,Number>::
reinit(const Mapping<dim>                                    &mapping,
       const std::vector<const DoFHandlerType *>             &dof_handler,
       const std::vector<const ConstraintMatrix *>           &constraint,
       const QuadratureType                                  &quad,
       const typename MatrixFree<dim,Number>::AdditionalData additional_data)
{
  std::vector<IndexSet> locally_owned_set =
    internal::MatrixFreeImplementation::extract_locally_owned_index_sets
    (dof_handler, additional_data.level_mg_handler);
  std::vector<hp::QCollection<1> > quad_hp;
  quad_hp.emplace_back (quad);
  internal_reinit(mapping, dof_handler,constraint,
                  locally_owned_set, quad_hp, additional_data);
}



template <int dim, typename Number>
template <typename DoFHandlerType, typename QuadratureType>
void MatrixFree<dim,Number>::
reinit(const Mapping<dim>                                   &mapping,
       const std::vector<const DoFHandlerType *>            &dof_handler,
       const std::vector<const ConstraintMatrix *>          &constraint,
       const std::vector<QuadratureType>                    &quad,
       const typename MatrixFree<dim,Number>::AdditionalData additional_data)
{
  std::vector<IndexSet> locally_owned_set =
    internal::MatrixFreeImplementation::extract_locally_owned_index_sets
    (dof_handler, additional_data.level_mg_handler);
  std::vector<hp::QCollection<1> > quad_hp;
  for (unsigned int q=0; q<quad.size(); ++q)
    quad_hp.emplace_back (quad[q]);
  internal_reinit(mapping, dof_handler,constraint,locally_owned_set,
                  quad_hp, additional_data);
}



template <int dim, typename Number>
template <typename DoFHandlerType, typename QuadratureType>
void MatrixFree<dim,Number>::
reinit(const Mapping<dim>                                    &mapping,
       const std::vector<const DoFHandlerType *>             &dof_handler,
       const std::vector<const ConstraintMatrix *>           &constraint,
       const std::vector<IndexSet>                           &locally_owned_set,
       const std::vector<QuadratureType>                     &quad,
       const typename MatrixFree<dim,Number>::AdditionalData additional_data)
{
  // find out whether we use a hp Quadrature or a standard quadrature
  std::vector<hp::QCollection<1> > quad_hp;
  for (unsigned int q=0; q<quad.size(); ++q)
    quad_hp.emplace_back (quad[q]);
  internal_reinit (mapping,
                   dof_handler,
                   constraint, locally_owned_set, quad_hp, additional_data);
}



// ------------------------------ implementation of loops --------------------

// internal helper functions that define how to call MPI data exchange
// functions: for generic vectors, do nothing at all. For distributed vectors,
// call update_ghost_values_start function and so on. If we have collections
// of vectors, just do the individual functions of the components. In order to
// keep ghost values consistent (whether we are in read or write mode), we
// also reset the values at the end. the whole situation is a bit complicated
// by the fact that we need to treat block vectors differently, which use some
// additional helper functions to select the blocks and template magic.
namespace internal
{
  template <int dim, typename Number>
  struct VectorDataExchange
  {
    // An arbitrary shift for communication to reduce the risk for accidental
    // interaction with other open communications that a user program might
    // set up
    static constexpr unsigned int channel_shift = 103;

    VectorDataExchange (const dealii::MatrixFree<dim,Number> &matrix_free,
                        const typename dealii::MatrixFree<dim,Number>::DataAccessOnFaces vector_face_access,
                        const unsigned int n_components)
      :
      matrix_free (matrix_free),
      vector_face_access (matrix_free.get_task_info().face_partition_data.empty() ?
                          dealii::MatrixFree<dim,Number>::DataAccessOnFaces::unspecified :
                          vector_face_access),
      ghosts_were_set (false)
#ifdef DEAL_II_WITH_MPI
      , tmp_data(n_components),
      requests(n_components)
#endif
    {
      (void)n_components;
      if (this->vector_face_access != dealii::MatrixFree<dim,Number>::DataAccessOnFaces::unspecified)
        for (unsigned int c=0; c<matrix_free.n_components(); ++c)
          AssertDimension(matrix_free.get_dof_info(c).vector_partitioner_face_variants.size(), 3);
    }

    ~VectorDataExchange ()
    {
#ifdef DEAL_II_WITH_MPI
      for (unsigned int i=0; i<tmp_data.size(); ++i)
        if (tmp_data[i] != nullptr)
          matrix_free.release_scratch_data_non_threadsafe(tmp_data[i]);
#endif
    }

    unsigned int find_vector_in_mf (const LinearAlgebra::distributed::Vector<Number> &vec,
                                    const bool check_global_compatibility = true) const
    {
      unsigned int mf_component = numbers::invalid_unsigned_int;
      (void)check_global_compatibility;
      for (unsigned int c=0; c<matrix_free.n_components(); ++c)
        if (
#ifdef DEBUG
          check_global_compatibility
          ?
          vec.get_partitioner()->is_globally_compatible(*matrix_free.get_dof_info(c).vector_partitioner)
          :
#endif
          vec.get_partitioner()->is_compatible(*matrix_free.get_dof_info(c).vector_partitioner))
          {
            mf_component = c;
            break;
          }
      return mf_component;
    }

    const Utilities::MPI::Partitioner &
    get_partitioner(const unsigned int mf_component) const
    {
      AssertDimension(matrix_free.get_dof_info(mf_component).vector_partitioner_face_variants.size(),3);
      if (vector_face_access == dealii::MatrixFree<dim,Number>::DataAccessOnFaces::none)
        return *matrix_free.get_dof_info(mf_component).vector_partitioner_face_variants[0];
      else if (vector_face_access == dealii::MatrixFree<dim,Number>::DataAccessOnFaces::values)
        return *matrix_free.get_dof_info(mf_component).vector_partitioner_face_variants[1];
      else
        return *matrix_free.get_dof_info(mf_component).vector_partitioner_face_variants[2];
    }

    void update_ghost_values_start(const unsigned int component_in_block_vector,
                                   const LinearAlgebra::distributed::Vector<Number> &vec)
    {
      (void)component_in_block_vector;
      bool ghosts_set = vec.has_ghost_elements();
      if (ghosts_set)
        ghosts_were_set = true;
      if (vector_face_access == dealii::MatrixFree<dim,Number>::DataAccessOnFaces::unspecified ||
          vec.size() == 0)
        vec.update_ghost_values_start(component_in_block_vector + channel_shift);
      else
        {
#ifdef DEAL_II_WITH_MPI
          const unsigned int mf_component = find_vector_in_mf(vec);
          if (&get_partitioner(mf_component) == matrix_free.get_dof_info(mf_component)
              .vector_partitioner.get())
            {
              vec.update_ghost_values_start(component_in_block_vector + channel_shift);
              return;
            }

          const Utilities::MPI::Partitioner &part = get_partitioner(mf_component);
          if (part.n_ghost_indices()==0 && part.n_import_indices()==0)
            return;

          tmp_data[component_in_block_vector] = matrix_free.acquire_scratch_data_non_threadsafe();
          tmp_data[component_in_block_vector]->resize_fast(part.n_import_indices());
          AssertDimension(requests.size(), tmp_data.size());

          part.export_to_ghosted_array_start
          (component_in_block_vector+channel_shift,
           ArrayView<const Number>(vec.begin(), part.local_size()),
           ArrayView<Number>(tmp_data[component_in_block_vector]->begin(),
                             part.n_import_indices()),
           ArrayView<Number>(const_cast<Number *>(vec.begin()) +
                             vec.get_partitioner()->local_size(),
                             vec.get_partitioner()->n_ghost_indices()),
           this->requests[component_in_block_vector]);
#endif
        }
    }

    void update_ghost_values_finish (const unsigned int component_in_block_vector,
                                     const LinearAlgebra::distributed::Vector<Number> &vec)
    {
      (void)component_in_block_vector;
      if (vector_face_access == dealii::MatrixFree<dim,Number>::DataAccessOnFaces::unspecified ||
          vec.size() == 0)
        vec.update_ghost_values_finish();
      else
        {
#ifdef DEAL_II_WITH_MPI

          AssertIndexRange(component_in_block_vector, tmp_data.size());
          AssertDimension(requests.size(), tmp_data.size());

          const unsigned int mf_component = find_vector_in_mf(vec);
          const Utilities::MPI::Partitioner &part = get_partitioner(mf_component);
          if (&part == matrix_free.get_dof_info(mf_component).vector_partitioner.get())
            {
              vec.update_ghost_values_finish();
              return;
            }

          if (part.n_ghost_indices()==0 && part.n_import_indices()==0)
            return;

          part.export_to_ghosted_array_finish
          (ArrayView<Number>(const_cast<Number *>(vec.begin()) +
                             vec.get_partitioner()->local_size(),
                             vec.get_partitioner()->n_ghost_indices()),
           this->requests[component_in_block_vector]);

          matrix_free.release_scratch_data_non_threadsafe(tmp_data[component_in_block_vector]);
          tmp_data[component_in_block_vector] = 0;
#endif
        }
    }

    void compress_start(const unsigned int component_in_block_vector,
                        LinearAlgebra::distributed::Vector<Number> &vec)
    {
      (void)component_in_block_vector;
      Assert(vec.has_ghost_elements() == false, ExcNotImplemented());
      if (vector_face_access == dealii::MatrixFree<dim,Number>::DataAccessOnFaces::unspecified ||
          vec.size() == 0)
        vec.compress_start(component_in_block_vector + channel_shift);
      else
        {
#ifdef DEAL_II_WITH_MPI

          const unsigned int mf_component = find_vector_in_mf(vec);
          const Utilities::MPI::Partitioner &part = get_partitioner(mf_component);
          if (&part == matrix_free.get_dof_info(mf_component).vector_partitioner.get())
            {
              vec.compress_start(component_in_block_vector + channel_shift);
              return;
            }

          if (part.n_ghost_indices()==0 && part.n_import_indices()==0)
            return;

          tmp_data[component_in_block_vector] = matrix_free.acquire_scratch_data_non_threadsafe();
          tmp_data[component_in_block_vector]->resize_fast(part.n_import_indices());
          AssertDimension(requests.size(), tmp_data.size());

          part.import_from_ghosted_array_start
          (dealii::VectorOperation::add,
           component_in_block_vector+channel_shift,
           ArrayView<Number>(vec.begin()+vec.get_partitioner()->local_size(),
                             vec.get_partitioner()->n_ghost_indices()),
           ArrayView<Number>(tmp_data[component_in_block_vector]->begin(),
                             part.n_import_indices()),
           this->requests[component_in_block_vector]);
#endif
        }
    }

    void compress_finish (const unsigned int component_in_block_vector,
                          LinearAlgebra::distributed::Vector<Number> &vec)
    {
      (void)component_in_block_vector;
      if (vector_face_access == dealii::MatrixFree<dim,Number>::DataAccessOnFaces::unspecified ||
          vec.size() == 0)
        vec.compress_finish(dealii::VectorOperation::add);
      else
        {
#ifdef DEAL_II_WITH_MPI
          AssertIndexRange(component_in_block_vector, tmp_data.size());
          AssertDimension(requests.size(), tmp_data.size());

          const unsigned int mf_component = find_vector_in_mf(vec);

          const Utilities::MPI::Partitioner &part = get_partitioner(mf_component);
          if (&part == matrix_free.get_dof_info(mf_component).vector_partitioner.get())
            {
              vec.compress_finish(dealii::VectorOperation::add);
              return;
            }

          if (part.n_ghost_indices()==0 && part.n_import_indices()==0)
            return;

          part.import_from_ghosted_array_finish
          (VectorOperation::add,
           ArrayView<const Number>(tmp_data[component_in_block_vector]->begin(),
                                   part.n_import_indices()),
           ArrayView<Number>(vec.begin(), part.local_size()),
           ArrayView<Number>(vec.begin()+vec.get_partitioner()->local_size(),
                             vec.get_partitioner()->n_ghost_indices()),
           this->requests[component_in_block_vector]);

          matrix_free.release_scratch_data_non_threadsafe(tmp_data[component_in_block_vector]);
          tmp_data[component_in_block_vector] = 0;
#endif
        }
    }

    void reset_ghost_values(const LinearAlgebra::distributed::Vector<Number> &vec) const
    {
      if (ghosts_were_set == true)
        return;

      if (vector_face_access == dealii::MatrixFree<dim,Number>::DataAccessOnFaces::unspecified ||
          vec.size() == 0)
        const_cast<LinearAlgebra::distributed::Vector<Number> &>(vec).zero_out_ghosts();
      else
        {
#ifdef DEAL_II_WITH_MPI
          AssertDimension(requests.size(), tmp_data.size());

          const unsigned int mf_component = find_vector_in_mf(vec);
          const Utilities::MPI::Partitioner &part = get_partitioner(mf_component);
          if (&part == matrix_free.get_dof_info(mf_component).vector_partitioner.get())
            const_cast<LinearAlgebra::distributed::Vector<Number> &>(vec).zero_out_ghosts();
          else if (part.n_ghost_indices() > 0)
            {
              for (std::vector<std::pair<unsigned int, unsigned int> >::const_iterator
                   my_ghosts = part.ghost_indices_within_larger_ghost_set().begin();
                   my_ghosts != part.ghost_indices_within_larger_ghost_set().end();
                   ++my_ghosts)
                for (unsigned int j=my_ghosts->first; j<my_ghosts->second; j++)
                  {
                    const_cast<LinearAlgebra::distributed::Vector<Number> &>(vec)
                    .local_element(j+part.local_size()) = 0.;
                  }
            }
#endif
        }
    }

    void zero_vector_region(const unsigned int range_index,
                            LinearAlgebra::distributed::Vector<Number> &vec) const
    {
      if (range_index == numbers::invalid_unsigned_int)
        vec = 0;
      else
        {
          const unsigned int mf_component = find_vector_in_mf(vec, false);
          const internal::MatrixFreeFunctions::DoFInfo &dof_info =
            matrix_free.get_dof_info(mf_component);
          Assert(dof_info.vector_zero_range_list_index.empty() == false,
                 ExcNotInitialized());

          Assert(vec.partitioners_are_compatible(*dof_info.vector_partitioner),
                 ExcInternalError());
          AssertIndexRange(range_index, dof_info.vector_zero_range_list_index.size()-1);
          for (unsigned int id=dof_info.vector_zero_range_list_index[range_index];
               id != dof_info.vector_zero_range_list_index[range_index+1]; ++id)
            {
              const unsigned int start_pos = dof_info.vector_zero_range_list[id]*
                                             internal::MatrixFreeFunctions::DoFInfo::chunk_size_zero_vector;
              const unsigned int end_pos = std::min((dof_info.vector_zero_range_list[id]+1)*
                                                    internal::MatrixFreeFunctions::DoFInfo::chunk_size_zero_vector,
                                                    dof_info.vector_partitioner->local_size()+
                                                    dof_info.vector_partitioner->n_ghost_indices());
              std::memset(vec.begin()+start_pos, 0, (end_pos-start_pos)*sizeof(Number));
            }
        }
    }

    const dealii::MatrixFree<dim,Number> &matrix_free;
    const typename dealii::MatrixFree<dim,Number>::DataAccessOnFaces vector_face_access;
    bool ghosts_were_set;
#ifdef DEAL_II_WITH_MPI
    std::vector<AlignedVector<Number> *> tmp_data;
    std::vector<std::vector<MPI_Request> > requests;
#endif
  };

  template <typename VectorStruct>
  unsigned int n_components (const VectorStruct &vec);

  template <typename VectorStruct>
  unsigned int n_components_block (const VectorStruct &vec,
                                   std::integral_constant<bool,true>)
  {
    unsigned int components = 0;
    for (unsigned int bl=0; bl<vec.n_blocks(); ++bl)
      components += n_components(vec.block(bl));
    return components;
  }

  template <typename VectorStruct>
  unsigned int n_components_block (const VectorStruct &,
                                   std::integral_constant<bool,false>)
  {
    return 1;
  }

  template <typename VectorStruct>
  unsigned int n_components (const VectorStruct &vec)
  {
    return n_components_block(vec, std::integral_constant<bool,IsBlockVector<VectorStruct>::value>());
  }

  template <typename VectorStruct>
  inline
  unsigned int n_components (const std::vector<VectorStruct> &vec)
  {
    unsigned int components = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      components += n_components_block(vec[comp], std::integral_constant<bool,IsBlockVector<VectorStruct>::value>());
    return components;
  }

  template <typename VectorStruct>
  inline
  unsigned int n_components (const std::vector<VectorStruct *> &vec)
  {
    unsigned int components = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      components += n_components_block(*vec[comp], std::integral_constant<bool,IsBlockVector<VectorStruct>::value>());
    return components;
  }

  template <int dim, typename VectorStruct, typename Number>
  void update_ghost_values_start_block (const VectorStruct &vec,
                                        const unsigned int channel,
                                        std::integral_constant<bool,true>,
                                        VectorDataExchange<dim,Number> &exchanger);
  template <int dim, typename VectorStruct, typename Number>
  void reset_ghost_values_block (const VectorStruct &vec,
                                 std::integral_constant<bool,true>,
                                 VectorDataExchange<dim,Number> &exchanger);
  template <int dim, typename VectorStruct, typename Number>
  void update_ghost_values_finish_block (const VectorStruct &vec,
                                         const unsigned int channel,
                                         std::integral_constant<bool,true>,
                                         VectorDataExchange<dim,Number> &exchanger);
  template <int dim, typename VectorStruct, typename Number>
  void compress_start_block (const VectorStruct &vec,
                             const unsigned int channel,
                             std::integral_constant<bool,true>,
                             VectorDataExchange<dim,Number> &exchanger);
  template <int dim, typename VectorStruct, typename Number>
  void compress_finish_block (const VectorStruct &vec,
                              const unsigned int channel,
                              std::integral_constant<bool,true>,
                              VectorDataExchange<dim,Number> &exchanger);
  template <int dim, typename VectorStruct, typename Number>
  void zero_vector_region_block (const unsigned int range_index,
                                 VectorStruct &,
                                 std::integral_constant<bool,true>,
                                 VectorDataExchange<dim,Number> &);

  template <int dim, typename VectorStruct, typename Number>
  void update_ghost_values_start_block (const VectorStruct &,
                                        const unsigned int ,
                                        std::integral_constant<bool,false>,
                                        VectorDataExchange<dim,Number> &)
  {}
  template <int dim, typename VectorStruct, typename Number>
  void reset_ghost_values_block (const VectorStruct &,
                                 std::integral_constant<bool,false>,
                                 VectorDataExchange<dim,Number> &)
  {}
  template <int dim, typename VectorStruct, typename Number>
  void update_ghost_values_finish_block (const VectorStruct &,
                                         const unsigned int ,
                                         std::integral_constant<bool,false>,
                                         VectorDataExchange<dim,Number> &)
  {}
  template <int dim, typename VectorStruct, typename Number>
  void compress_start_block (const VectorStruct &,
                             const unsigned int ,
                             std::integral_constant<bool,false>,
                             VectorDataExchange<dim,Number> &)
  {}
  template <int dim, typename VectorStruct, typename Number>
  void compress_finish_block (const VectorStruct &,
                              const unsigned int ,
                              std::integral_constant<bool,false>,
                              VectorDataExchange<dim,Number> &)
  {}
  template <int dim, typename VectorStruct, typename Number>
  void zero_vector_region_block (const unsigned int range_index,
                                 VectorStruct &vec,
                                 std::integral_constant<bool,false>,
                                 VectorDataExchange<dim,Number> &)
  {
    if (range_index == 0 || range_index == numbers::invalid_unsigned_int)
      vec = 0;
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_start (const VectorStruct &vec,
                                  VectorDataExchange<dim,Number> &exchanger,
                                  const unsigned int channel = 0)
  {
    update_ghost_values_start_block(vec, channel,
                                    std::integral_constant<bool,
                                    IsBlockVector<VectorStruct>::value>(),
                                    exchanger);
  }



  template <int dim, typename Number, typename Number2>
  inline
  void update_ghost_values_start (const LinearAlgebra::distributed::Vector<Number> &vec,
                                  VectorDataExchange<dim,Number2> &exchanger,
                                  const unsigned int channel = 0)
  {
    exchanger.update_ghost_values_start(channel, vec);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_start (const std::vector<VectorStruct> &vec,
                                  VectorDataExchange<dim,Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        update_ghost_values_start(vec[comp], exchanger, component_index);
        component_index += n_components(vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_start (const std::vector<VectorStruct *> &vec,
                                  VectorDataExchange<dim,Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        update_ghost_values_start(*vec[comp], exchanger, component_index);
        component_index += n_components(*vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_start_block (const VectorStruct &vec,
                                        const unsigned int channel,
                                        std::integral_constant<bool,true>,
                                        VectorDataExchange<dim,Number> &exchanger)
  {
    for (unsigned int i=0; i<vec.n_blocks(); ++i)
      update_ghost_values_start(vec.block(i), exchanger, channel+i);
  }



  // if the input vector did not have ghosts imported, clear them here again
  // in order to avoid subsequent operations e.g. in linear solvers to work
  // with ghosts all the time
  template <int dim, typename VectorStruct, typename Number>
  inline
  void reset_ghost_values (const VectorStruct &vec,
                           VectorDataExchange<dim,Number> &exchanger)
  {
    reset_ghost_values_block(vec,
                             std::integral_constant<bool,
                             IsBlockVector<VectorStruct>::value>(),
                             exchanger);
  }



  template <int dim, typename Number, typename Number2>
  inline
  void reset_ghost_values (const LinearAlgebra::distributed::Vector<Number> &vec,
                           VectorDataExchange<dim,Number2> &exchanger)
  {
    exchanger.reset_ghost_values(vec);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void reset_ghost_values (const std::vector<VectorStruct> &vec,
                           VectorDataExchange<dim,Number> &exchanger)
  {
    for (unsigned int comp=0; comp<vec.size(); comp++)
      reset_ghost_values(vec[comp], exchanger);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void reset_ghost_values (const std::vector<VectorStruct *> &vec,
                           VectorDataExchange<dim,Number> &exchanger)
  {
    for (unsigned int comp=0; comp<vec.size(); comp++)
      reset_ghost_values(*vec[comp], exchanger);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void reset_ghost_values_block (const VectorStruct &vec,
                                 std::integral_constant<bool,true>,
                                 VectorDataExchange<dim,Number> &exchanger)
  {
    for (unsigned int i=0; i<vec.n_blocks(); ++i)
      reset_ghost_values(vec.block(i), exchanger);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_finish (const VectorStruct &vec,
                                   VectorDataExchange<dim,Number> &exchanger,
                                   const unsigned int channel = 0)
  {
    update_ghost_values_finish_block(vec, channel,
                                     std::integral_constant<bool,
                                     IsBlockVector<VectorStruct>::value>(),
                                     exchanger);
  }



  template <int dim, typename Number, typename Number2>
  inline
  void update_ghost_values_finish (const LinearAlgebra::distributed::Vector<Number> &vec,
                                   VectorDataExchange<dim,Number2> &exchanger,
                                   const unsigned int channel = 0)
  {
    exchanger.update_ghost_values_finish(channel, vec);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_finish (const std::vector<VectorStruct> &vec,
                                   VectorDataExchange<dim,Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        update_ghost_values_finish(vec[comp], exchanger, component_index);
        component_index += n_components(vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_finish (const std::vector<VectorStruct *> &vec,
                                   VectorDataExchange<dim,Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        update_ghost_values_finish(*vec[comp], exchanger, component_index);
        component_index += n_components(*vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void update_ghost_values_finish_block (const VectorStruct &vec,
                                         const unsigned int channel,
                                         std::integral_constant<bool,true>,
                                         VectorDataExchange<dim,Number> &exchanger)
  {
    for (unsigned int i=0; i<vec.n_blocks(); ++i)
      update_ghost_values_finish(vec.block(i), exchanger, channel+i);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_start (VectorStruct &vec,
                       VectorDataExchange<dim, Number> &exchanger,
                       const unsigned int channel = 0)
  {
    compress_start_block (vec, channel,
                          std::integral_constant<bool,
                          IsBlockVector<VectorStruct>::value>(),
                          exchanger);
  }



  template <int dim, typename Number, typename Number2>
  inline
  void compress_start (LinearAlgebra::distributed::Vector<Number> &vec,
                       VectorDataExchange<dim,Number2> &exchanger,
                       const unsigned int           channel = 0)
  {
    exchanger.compress_start(channel, vec);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_start (std::vector<VectorStruct> &vec,
                       VectorDataExchange<dim, Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        compress_start(vec[comp], exchanger, component_index);
        component_index += n_components(vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_start (std::vector<VectorStruct *> &vec,
                       VectorDataExchange<dim, Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        compress_start(*vec[comp], exchanger, component_index);
        component_index += n_components(*vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_start_block (VectorStruct      &vec,
                             const unsigned int channel,
                             std::integral_constant<bool,true>,
                             VectorDataExchange<dim, Number> &exchanger)
  {
    for (unsigned int i=0; i<vec.n_blocks(); ++i)
      compress_start(vec.block(i), exchanger, channel+i);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_finish (VectorStruct &vec,
                        VectorDataExchange<dim, Number> &exchanger,
                        const unsigned int channel = 0)
  {
    compress_finish_block(vec, channel,
                          std::integral_constant<bool,
                          IsBlockVector<VectorStruct>::value>(),
                          exchanger);
  }



  template <int dim, typename Number, typename Number2>
  inline
  void compress_finish (LinearAlgebra::distributed::Vector<Number> &vec,
                        VectorDataExchange<dim, Number2> &exchanger,
                        const unsigned int channel = 0)
  {
    exchanger.compress_finish(channel, vec);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_finish (std::vector<VectorStruct> &vec,
                        VectorDataExchange<dim, Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        compress_finish(vec[comp], exchanger, component_index);
        component_index += n_components(vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_finish (std::vector<VectorStruct *> &vec,
                        VectorDataExchange<dim, Number> &exchanger)
  {
    unsigned int component_index = 0;
    for (unsigned int comp=0; comp<vec.size(); comp++)
      {
        compress_finish(*vec[comp], exchanger, component_index);
        component_index += n_components(*vec[comp]);
      }
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void compress_finish_block (VectorStruct &vec,
                              const unsigned int channel,
                              std::integral_constant<bool,true>,
                              VectorDataExchange<dim, Number> &exchanger)
  {
    for (unsigned int i=0; i<vec.n_blocks(); ++i)
      compress_finish(vec.block(i), exchanger, channel+i);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void zero_vector_region (const unsigned int range_index,
                           VectorStruct &vec,
                           VectorDataExchange<dim, Number> &exchanger)
  {
    zero_vector_region_block(range_index, vec,
                             std::integral_constant<bool,
                             IsBlockVector<VectorStruct>::value>(),
                             exchanger);
  }



  template <int dim, typename Number, typename Number2>
  inline
  void zero_vector_region (const unsigned int range_index,
                           LinearAlgebra::distributed::Vector<Number> &vec,
                           VectorDataExchange<dim, Number2> &exchanger)
  {
    exchanger.zero_vector_region(range_index, vec);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void zero_vector_region (const unsigned int range_index,
                           std::vector<VectorStruct> &vec,
                           VectorDataExchange<dim, Number> &exchanger)
  {
    for (unsigned int comp=0; comp<vec.size(); comp++)
      zero_vector_region(range_index, vec[comp], exchanger);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void zero_vector_region (const unsigned int range_index,
                           std::vector<VectorStruct *> &vec,
                           VectorDataExchange<dim, Number> &exchanger)
  {
    for (unsigned int comp=0; comp<vec.size(); comp++)
      zero_vector_region(range_index, *vec[comp], exchanger);
  }



  template <int dim, typename VectorStruct, typename Number>
  inline
  void zero_vector_region_block (const unsigned int range_index,
                                 VectorStruct &vec,
                                 std::integral_constant<bool,true>,
                                 VectorDataExchange<dim, Number> &exchanger)
  {
    for (unsigned int i=0; i<vec.n_blocks(); ++i)
      zero_vector_region(range_index, vec.block(i), exchanger);
  }



  namespace MatrixFreeFunctions
  {
    // struct to select between a const interface and a non-const interface
    // for MFWorker
    template <typename, typename, typename, typename, bool>
    struct InterfaceSelector
    {};

    // Version of constant functions
    template <typename MF, typename InVector, typename OutVector, typename Container>
    struct InterfaceSelector<MF, InVector, OutVector, Container, true>
    {
      typedef void (Container::*function_type)
      (const MF &, OutVector &, const InVector &,
       const std::pair<unsigned int, unsigned int> &)const;
    };

    // Version for non-constant functions
    template <typename MF, typename InVector, typename OutVector, typename Container>
    struct InterfaceSelector<MF, InVector, OutVector, Container, false>
    {
      typedef void (Container::*function_type)
      (const MF &, OutVector &, const InVector &,
       const std::pair<unsigned int, unsigned int> &);
    };
  }



  // A implementation class for the worker object that runs the various
  // operations we want to perform during the matrix-free loop
  template <typename MF, typename InVector, typename OutVector,
            typename Container, bool is_constant>
  class MFWorker : public MFWorkerInterface
  {
  public:
    // A typedef to make the arguments further down more readable
    typedef typename MatrixFreeFunctions::InterfaceSelector
    <MF,InVector,OutVector,Container,is_constant>::function_type function_type;

    // constructor, binds all the arguments to this class
    MFWorker (const MF &matrix_free,
              const InVector &src,
              OutVector &dst,
              const bool zero_dst_vector_setting,
              const Container &container,
              function_type cell_function,
              function_type face_function,
              function_type boundary_function,
              const typename MF::DataAccessOnFaces src_vector_face_access =
                MF::DataAccessOnFaces::none,
              const typename MF::DataAccessOnFaces dst_vector_face_access =
                MF::DataAccessOnFaces::none)
      :
      matrix_free (matrix_free),
      container (const_cast<Container &>(container)),
      cell_function (cell_function),
      face_function (face_function),
      boundary_function (boundary_function),
      src (src),
      dst (dst),
      src_data_exchanger (matrix_free, src_vector_face_access,
                          n_components(src)),
      dst_data_exchanger (matrix_free, dst_vector_face_access,
                          n_components(dst)),
      src_and_dst_are_same (PointerComparison::equal(&src, &dst)),
      zero_dst_vector_setting(zero_dst_vector_setting && !src_and_dst_are_same)
    {}

    // Runs the cell work. If no function is given, nothing is done
    virtual void cell(const std::pair<unsigned int,unsigned int> &cell_range)
    {
      if (cell_function != nullptr && cell_range.second > cell_range.first)
        (container.*cell_function)(matrix_free, this->dst, this->src, cell_range);
    }

    // Runs the assembler on interior faces. If no function is given, nothing
    // is done
    virtual void face(const std::pair<unsigned int,unsigned int> &face_range)
    {
      if (face_function != nullptr && face_range.second > face_range.first)
        (container.*face_function)(matrix_free, this->dst, this->src, face_range);
    }

    // Runs the assembler on boundary faces. If no function is given, nothing
    // is done
    virtual void boundary(const std::pair<unsigned int,unsigned int> &face_range)
    {
      if (boundary_function != nullptr && face_range.second > face_range.first)
        (container.*boundary_function)(matrix_free, this->dst, this->src, face_range);
    }

    // Starts the communication for the update ghost values operation. We
    // cannot call this update if ghost and destination are the same because
    // that would introduce spurious entries in the destination (there is also
    // the problem that reading from a vector that we also write to is usually
    // not intended in case there is overlap, but this is up to the
    // application code to decide and we cannot catch this case here).
    virtual void vector_update_ghosts_start()
    {
      if (!src_and_dst_are_same)
        internal::update_ghost_values_start(src, src_data_exchanger);
    }

    // Finishes the communication for the update ghost values operation
    virtual void vector_update_ghosts_finish()
    {
      if (!src_and_dst_are_same)
        internal::update_ghost_values_finish(src, src_data_exchanger);
    }

    // Starts the communication for the vector compress operation
    virtual void vector_compress_start()
    {
      internal::compress_start(dst, dst_data_exchanger);
    }

    // Finishes the communication for the vector compress operation
    virtual void vector_compress_finish()
    {
      internal::compress_finish(dst, dst_data_exchanger);
      if (!src_and_dst_are_same)
        internal::reset_ghost_values(src, src_data_exchanger);
    }

    // Zeros the given input vector
    virtual void zero_dst_vector_range(const unsigned int range_index)
    {
      if (zero_dst_vector_setting)
        internal::zero_vector_region(range_index, dst, dst_data_exchanger);
    }

  private:
    const MF       &matrix_free;
    Container      &container;
    function_type   cell_function;
    function_type   face_function;
    function_type   boundary_function;

    const InVector &src;
    OutVector      &dst;
    VectorDataExchange<MF::dimension,typename MF::value_type> src_data_exchanger;
    VectorDataExchange<MF::dimension,typename MF::value_type> dst_data_exchanger;
    const bool      src_and_dst_are_same;
    const bool      zero_dst_vector_setting;
  };



  /**
   * An internal class to convert three function pointers to the
   * scheme with virtual functions above.
   */
  template <class MF, typename InVector, typename OutVector>
  struct MFClassWrapper
  {
    typedef std::function<void (const MF &, OutVector &, const InVector &,
                                const std::pair<unsigned int, unsigned int> &)> function_type;

    MFClassWrapper (const function_type cell,
                    const function_type face,
                    const function_type boundary)
      :
      cell (cell),
      face (face),
      boundary (boundary)
    {}

    void cell_integrator (const MF &mf, OutVector &dst, const InVector &src,
                          const std::pair<unsigned int, unsigned int> &range) const
    {
      if (cell)
        cell(mf, dst, src, range);
    }

    void face_integrator (const MF &mf, OutVector &dst, const InVector &src,
                          const std::pair<unsigned int, unsigned int> &range) const
    {
      if (face)
        face(mf, dst, src, range);
    }

    void boundary_integrator (const MF &mf, OutVector &dst, const InVector &src,
                              const std::pair<unsigned int, unsigned int> &range) const
    {
      if (boundary)
        boundary(mf, dst, src, range);
    }

    const function_type cell;
    const function_type face;
    const function_type boundary;
  };

} // end of namespace internal



template <int dim, typename Number>
template <typename OutVector, typename InVector>
inline
void
MatrixFree<dim, Number>::cell_loop
(const std::function<void (const MatrixFree<dim,Number> &,
                           OutVector &,
                           const InVector &,
                           const std::pair<unsigned int,
                           unsigned int> &)> &cell_operation,
 OutVector       &dst,
 const InVector  &src,
 const bool       zero_dst_vector) const
{
  typedef internal::MFClassWrapper<MatrixFree<dim, Number>, InVector, OutVector> Wrapper;
  Wrapper wrap (cell_operation, nullptr, nullptr);
  internal::MFWorker<MatrixFree<dim, Number>, InVector, OutVector, Wrapper, true>
  worker(*this, src, dst, zero_dst_vector, wrap, &Wrapper::cell_integrator,
         &Wrapper::face_integrator, &Wrapper::boundary_integrator);

  task_info.loop (worker);
}



template <int dim, typename Number>
template <typename OutVector, typename InVector>
inline
void
MatrixFree<dim, Number>::loop
(const std::function<void (const MatrixFree<dim,Number> &,
                           OutVector &,
                           const InVector &,
                           const std::pair<unsigned int,
                           unsigned int> &)> &cell_operation,
 const std::function<void (const MatrixFree<dim,Number> &,
                           OutVector &,
                           const InVector &,
                           const std::pair<unsigned int,
                           unsigned int> &)> &face_operation,
 const std::function<void (const MatrixFree<dim,Number> &,
                           OutVector &,
                           const InVector &,
                           const std::pair<unsigned int,
                           unsigned int> &)> &boundary_operation,
 OutVector       &dst,
 const InVector  &src,
 const bool       zero_dst_vector,
 const DataAccessOnFaces dst_vector_face_access,
 const DataAccessOnFaces src_vector_face_access) const
{
  typedef internal::MFClassWrapper<MatrixFree<dim, Number>, InVector, OutVector> Wrapper;
  Wrapper wrap (cell_operation, face_operation, boundary_operation);
  internal::MFWorker<MatrixFree<dim, Number>, InVector, OutVector, Wrapper, true>
  worker(*this, src, dst, zero_dst_vector, wrap, &Wrapper::cell_integrator,
         &Wrapper::face_integrator, &Wrapper::boundary_integrator,
         src_vector_face_access, dst_vector_face_access);

  task_info.loop(worker);
}



template <int dim, typename Number>
template <typename CLASS, typename OutVector, typename InVector>
inline
void
MatrixFree<dim,Number>::cell_loop
(void (CLASS::*function_pointer)(const MatrixFree<dim,Number> &,
                                 OutVector &,
                                 const InVector &,
                                 const std::pair<unsigned int, unsigned int> &)const,
 const CLASS    *owning_class,
 OutVector      &dst,
 const InVector &src,
 const bool       zero_dst_vector) const
{
  internal::MFWorker<MatrixFree<dim, Number>, InVector, OutVector, CLASS, true>
  worker(*this, src, dst, zero_dst_vector, *owning_class, function_pointer, nullptr, nullptr);
  task_info.loop(worker);
}



template <int dim, typename Number>
template <typename CLASS, typename OutVector, typename InVector>
inline
void
MatrixFree<dim,Number>::loop
(void (CLASS::*cell_operation)(const MatrixFree<dim,Number> &,
                               OutVector &,
                               const InVector &,
                               const std::pair<unsigned int, unsigned int> &)const,
 void (CLASS::*face_operation)(const MatrixFree<dim,Number> &,
                               OutVector &,
                               const InVector &,
                               const std::pair<unsigned int, unsigned int> &)const,
 void (CLASS::*boundary_operation)(const MatrixFree<dim,Number> &,
                                   OutVector &,
                                   const InVector &,
                                   const std::pair<unsigned int, unsigned int> &)const,
 const CLASS    *owning_class,
 OutVector      &dst,
 const InVector &src,
 const bool       zero_dst_vector,
 const DataAccessOnFaces dst_vector_face_access,
 const DataAccessOnFaces src_vector_face_access) const
{
  internal::MFWorker<MatrixFree<dim, Number>, InVector, OutVector, CLASS, true>
  worker(*this, src, dst, zero_dst_vector, *owning_class, cell_operation, face_operation,
         boundary_operation, src_vector_face_access, dst_vector_face_access);
  task_info.loop(worker);
}



template <int dim, typename Number>
template <typename CLASS, typename OutVector, typename InVector>
inline
void
MatrixFree<dim,Number>::cell_loop
(void(CLASS::*function_pointer)(const MatrixFree<dim,Number> &,
                                OutVector &,
                                const InVector &,
                                const std::pair<unsigned int, unsigned int> &),
 CLASS          *owning_class,
 OutVector      &dst,
 const InVector &src,
 const bool       zero_dst_vector) const
{
  internal::MFWorker<MatrixFree<dim, Number>, InVector, OutVector, CLASS, false>
  worker(*this, src, dst, zero_dst_vector, *owning_class, function_pointer, nullptr, nullptr);
  task_info.loop(worker);
}



template <int dim, typename Number>
template <typename CLASS, typename OutVector, typename InVector>
inline
void
MatrixFree<dim,Number>::loop
(void(CLASS::*cell_operation)(const MatrixFree<dim,Number> &,
                              OutVector &,
                              const InVector &,
                              const std::pair<unsigned int, unsigned int> &),
 void(CLASS::*face_operation)(const MatrixFree<dim,Number> &,
                              OutVector &,
                              const InVector &,
                              const std::pair<unsigned int, unsigned int> &),
 void(CLASS::*boundary_operation)(const MatrixFree<dim,Number> &,
                                  OutVector &,
                                  const InVector &,
                                  const std::pair<unsigned int, unsigned int> &),
 CLASS          *owning_class,
 OutVector      &dst,
 const InVector &src,
 const bool       zero_dst_vector,
 const DataAccessOnFaces dst_vector_face_access,
 const DataAccessOnFaces src_vector_face_access) const
{
  internal::MFWorker<MatrixFree<dim, Number>, InVector, OutVector, CLASS, false>
  worker(*this, src, dst, zero_dst_vector, *owning_class, cell_operation,
         face_operation, boundary_operation,
         src_vector_face_access, dst_vector_face_access);
  task_info.loop(worker);
}


#endif  // ifndef DOXYGEN



DEAL_II_NAMESPACE_CLOSE

#endif
