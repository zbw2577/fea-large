#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <assert.h>
#ifdef USE_EXPAT
#include <expat.h>
#endif
#ifdef DMALLOC
#include "dmalloc.h"
#endif

/*************************************************************/
/* Type and constants definitions                            */

/* BOOL type as usual */
typedef int BOOL;
#define FALSE 0
#define TRUE 1

#define MAX_DOF 3
#define MAX_MATERIAL_PARAMETERS 10

/*
 * SLAE solver constants
 * possibly shall go to the initial data in future
 */
#define MAX_ITER 10000
#define TOLERANCE 1e-10


/* Redefine type of the floating point values */
#ifdef SINGLE
typedef float real;
#else /* not SINGLE */
typedef double real;
#endif /* not SINGLE */

/* Equals macro for real values */
#ifdef SINGLE
#define EQL(x,y) (fabs((x)-(y))<= FLT_MIN)
#else
#define EQL(x,y) (fabs((x)-(y))<= DBL_MIN)
#endif

#define DELTA(i,j) ((i)==(j) ? 1 : 0)


/*
 * A pointer to the the isoparametric shape
 * function
 */
typedef real (*isoform_t)(int i,real r,real s,real t);

/*
 * A pointer to the derivative of the isoparametric shape
 * function
 */
typedef real (*disoform_t)(int shape,int dof,real r,real s,real t);

/*************************************************************/
/* Global variables                                          */

extern int errno;
struct fea_solver_tag* global_solver;

/*
 * arrays of gauss nodes with coefficients                   
 * layout: [number_of_nodes x 4], with values:               
 * {weight, r,s,t}                                           
 * per gauss node. For 2d cases t = 0
 * Note what divisor 6 for tetraheadras and 2 for triangles
 * shall be already taken into account in weights.
 * See below.
 */

/* Element: TETRAHEDRA10, 4 nodes */
real gauss_nodes4_tetr10[4][4] = { {(1/4.)/6.,        /* weight */
                                    0.58541020,  /* a */
                                    0.13819660,  /* b */
                                    0.13819660}, /* b */
                                   {(1/4.)/6.,
                                    0.13819660,  /* b */
                                    0.58541020,  /* a */
                                    0.13819660}, /* b */
                                   {(1/4.)/6.,
                                    0.13819660,  /* b */
                                    0.13819660,  /* b */
                                    0.58541020}, /* a */
                                   {(1/4.)/6.,
                                    0.13819660,  /* b */
                                    0.13819660,  /* b */
                                    0.13819660}  /* b */
};
/* Element: TETRAHEDRA10, 5 nodes */
real gauss_nodes5_tetr10[5][4] = { {(-4/5.)/6., 1/4., 1/4., 1/4.},
                                   {(9/20.)/6., 1/2., 1/6., 1/6.},
                                   {(9/20.)/6., 1/6., 1/2., 1/6.},
                                   {(9/20.)/6., 1/6., 1/6., 1/2.},
                                   {(9/20.)/6., 1/6., 1/6., 1/6.} };



typedef enum task_type_enum {
  /* PLANE_STRESS, PLANE_STRAIN, AXISYMMETRIC,  */
  CARTESIAN3D
} task_type;

typedef enum model_type_enum {
  MODEL_A5,
  MODEL_COMPRESSIBLE_NEOHOOKEAN
} model_type;
  
typedef enum element_type_enum {
  /* TRIANGLE3, TRIANGLE6,TETRAHEDRA4, */
  TETRAHEDRA10
} element_type;

typedef enum prescribed_boundary_type_enum {
  FREE = 0,                    /* free */
  PRESCRIBEDX = 1,             /* x prescribed */
  PRESCRIBEDY = 2,             /* y prescribed */
  PRESCRIBEDXY = 3,            /* x, y prescribed */
  PRESCRIBEDZ = 4,             /* z prescribed*/
  PRESCRIBEDXZ = 5,            /* x, z prescribed*/
  PRESCRIBEDYZ = 6,            /* y, z prescribed*/
  PRESCRIBEDXYZ = 7            /* x, y, z prescribed.*/
} prescribed_boundary_type;


typedef struct fea_material_model_tag {
  model_type model;                         /* model type */
  real parameters[MAX_MATERIAL_PARAMETERS]; /* model material parameters */
  int parameters_count;                     /* number of material params */
} fea_model;

/*
 * Task type declaration.
 * Defines an input parameters for the task, independent of 
 * the input geometry and loads
 */
typedef struct fea_task_tag {
  task_type type;               /* type of the task to solve */
  fea_model model;              /* material model */
  unsigned char dof;            /* number of degree of freedom */
  element_type ele_type;        /* type of the element */
  int load_increments_count;    /* number of load increments */
  real desired_tolerance;       /* desired energy tolerance */
  int linesearch_max;           /* maximum number of line searches */
  int arclength_max;            /* maximum number of arc lenght searches */
  BOOL modified_newton;         /* use modified Newton's method or not */

} fea_task;


/* Calculated solution parameters */
typedef struct fea_solution_params_tag {
  int nodes_per_element;        /* number of nodes defined in element 
                                   based on fea_task::ele_type */
  int gauss_nodes_count;        /* number of gauss nodes per element */
} fea_solution_params;

/*************************************************************/
/* Input geometry parameters                                 */

/* An array of nodes. */
typedef struct nodes_array_tag {
  int nodes_count;              /* number of input nodes */
  real **nodes;         /* nodes array,sized as nodes_count x MAX_DOF
                                 * so access is  nodes[node_number][dof] */
} nodes_array;

/* An array of elements */
typedef struct elements_array_tag {
  int elements_count;           /* number of elements */
  int **elements;               /* elements array, each line represents an
                                 * element. Element is an array of node
                                 * indexes
                                 */
} elements_array;

/* Particular prescribed boundary node */
typedef struct prescibed_boundary_node_tag {
  int node_number;
  real values[MAX_DOF];
  prescribed_boundary_type type;
} prescibed_boundary_node;

/*
 * An array of prescribed boundary conditions
 * either fixed and with prescribed displacements
 */
typedef struct prescribed_boundary_array_tag {
  int prescribed_nodes_count;
  prescibed_boundary_node *prescribed_nodes;
} prescribed_boundary_array;


/*************************************************************/
/* Application-specific structures                           */

/* Structure describing information for the gauss node
 * depending on number of shape functions N per element
 * TODO: add tables with layouts in comments */
typedef struct gauss_node_tag {
  real weight;                /* weight for the integration */
  real *forms;                /* shape function values for gauss node, N */
  real **dforms;              /* derivatives of shape functions with
                               * respect to d.o.f.
                               * Rows represent d.o.f, columns represent
                               * derivatives in nodes */
} gauss_node;

/*
 * database of elements
 * Contains all gauss nodes for elements together with
 * derivatives
 */
typedef struct elements_database_tag {
  real (*gauss_nodes_data)[4];  /* pointer to an array of gauss
                                   * coefficients and weights */  
  gauss_node **gauss_nodes;   /* Gauss nodes array */
} elements_database;

/*
 * Matrix of gradients of the shape functions with respect to
 * global coordinates
 * Calculated in one node(gauss node) of an element
 * Layout: [d.o.f x nodes_count]
 *              dN_j(r,s,t)
 * grad[i][j] = -----------
 *                  dX_i
 * 
 * X_1 = x, X_2 = y, X_3 = z coordinates
 * 
 */
typedef struct shape_gradients_tag {
  real **grad;                  /* array of derivatives of shape functions
                                 * [dof x nodes_per_element] */
  real detJ;                    /* determinant of Jacobi matrix */
} shape_gradients;


/*
 * Sparse matrix row/column storage array
 */
typedef struct indexed_array_tag {
  int width;
  int last_index;
  int *indexes;
  real *values;
} indexed_array;

/*
 * Sparse matrix row storage 
 * Internal format based on CRS
 */
typedef struct sparse_matrix_tag {
  int rows_count;
  int cols_count;
  indexed_array* rows;
} sparse_matrix;


/*
 * Sparse matrix CSLR format
 * used in sparse iterative solvers
 * Constructed from Sparse Matrix in assumption of the symmetric
 * matrix portrait
 */
typedef struct sparse_matrix_skyline_tag {
  int rows_count;
  int cols_count;
  int nonzeros;                 /* number of nonzero elements in matrix */
  int triangle_nonzeros_count;  /* number of nonzero elements in
                                 * upper or lower triangles */
  real *diag;                   /* rows_count elements in matrix diagonal */
  real *lower_triangle;         /* nonzero elements of the lower triangle */
  real *upper_triangle;         /* nonzero elements of the upper triangle */
  int *jptr;                    /* array of column/row indexes of the
                                 * lower/upper triangles */
  int *iptr;                    /* array of row/column offsets in jptr
                                 * for lower or upper triangles */
} sparse_matrix_skyline;

/*
 * A main application structure which shall contain all
 * data necessary for solution
 */
typedef struct fea_solver_tag {
  fea_task *task;               
  fea_solution_params *fea_params; 
  nodes_array *nodes;              
  elements_array *elements;
  prescribed_boundary_array *presc_boundary;
  elements_database elements_db;  /* array of pre-constructed
                                   * values of derivatives of the
                                   * isoparametric shape functions
                                   * in gauss nodes */
  disoform_t dshape;              /* a function pointer to derivative of the
                                   * shape function */
  isoform_t shape;                /* a function pointer to the shape
                                   * function */
  sparse_matrix global_mtx;        /* global stiffness matrix */
  real* global_forces_vct;      /* external forces vector */
  real* global_solution_vct;    /* vector of global solution */
} fea_solver;



/*************************************************************/
/* Functions declarations                                    */


/*
 * Load initial data from file
 */
BOOL initial_data_load(char *filename,
                       fea_task **task,
                       fea_solution_params **fea_params,
                       nodes_array **nodes,
                       elements_array **elements,
                       prescribed_boundary_array **presc_boundary);


/*************************************************************/
/* Allocators for internal data structures                   */

/*************************************************************/
/* Allocators for structures with a data from file           */

/* Initializa fea task structure and fill with default values */
static fea_task* new_fea_task();
/* Initializes fea solution params with default values */
static fea_solution_params* new_fea_solution_params();
/* Initialize nodes array but not initialize particular arrays  */
static nodes_array* new_nodes_array();
/* Initialize elements array but not initialize particular elements */
static elements_array* new_elements_array();
/* Initialize boundary nodes array but not initialize particular nodes */
static prescribed_boundary_array* new_prescribed_boundary_array();

/*
 * Constructor for the main application structure
 * all parameters shall be properly constructed and initialized
 * with data from file
 */
static fea_solver* new_fea_solver(fea_task *task,
                                  fea_solution_params *fea_params,
                                  nodes_array *nodes,
                                  elements_array *elements,
                                  prescribed_boundary_array *prscs_boundary);


/*************************************************************/
/* Deallocators for internal data structures                 */

static void free_fea_solution_params(fea_solution_params* params);
static void free_fea_task(fea_task* task);
static void free_nodes_array(nodes_array* nodes);
static void free_elements_array(elements_array *elements);
static void free_prescribed_boundary_array(prescribed_boundary_array* presc);

/*
 * Destructor for the main solver
 * Will also clear all aggregated structures
 */
static void free_fea_solver(fea_solver* solver);


/*************************************************************/
/* Sparse matrix operations                                  */

/* indexed_arrays operations */
/* Swap i and j elements in the indexed array.
 * Used in indexed_array_sort*/
void indexed_array_swap(indexed_array* self,int i, int j);
/* Performs in-place sort of the indexed array */
void indexed_array_sort(indexed_array* self, int l, int r);


/*
 * Initializer for a sparse matrix with specified rows and columns
 * number.
 * This function doesn't allocate the memory for the matrix itself;
 * only for its structures. Matrix mtx shall be already allocated
 * bandwdith - is a start bandwidth of a matrix row
 */
static void init_sparse_matrix(sparse_matrix* mtx,
                           int rows,
                           int cols,
                           int bandwidth);
/*
 * Destructor for a sparse matrix
 * This function doesn't deallocate memory for the matrix itself,
 * only for its structures.
 */
static void free_sparse_matrix(sparse_matrix* mtx);

/*
 * Construct CSLR sparse matrix based on sparse_matrix format
 * mtx - is the (reordered) sparse matrix to take data from
 * Acts as a copy-constructor
 */
static void init_sparse_matrix_skyline(sparse_matrix_skyline* self,
                                       sparse_matrix* mtx);
/*
 * Destructor for a sparse matrix in CSLR format
 * This function doesn't deallocate memory for the matrix itself,
 * only for its structures.
 */
static void free_sparse_matrix_skyline(sparse_matrix_skyline* self);

/* getters/setters for a sparse matrix */

/* returns a pointer to the specific element
 * zero pointer if not found */
static real* sparse_matrix_element(sparse_matrix* self,int i, int j);
/* adds an element value to the matrix node (i,j) */
static void sparse_matrix_element_add(sparse_matrix* self,int i, int j, real value);

/* rearrange columns of a matrix to prepare for solving SLAE */
static void sparse_matrix_reorder(sparse_matrix* self);

/*
 * Implements BLAS level 2 function SAXPY: y = A*x+b
 * All vectors shall be already allocated
static void sparse_matrix_saxpy((sparse_matrix* self,real* b,real* x, real* y);
 */

/* Matrix-vector multiplication
 * y = A*x*/
static void sparse_matrix_mv(sparse_matrix* self,real* x, real* y);

/*
 * Solve SLAE for a matrix self with right-part b
 * Store results to the vector x. It shall be already allocated
 */
static void sparse_matrix_solve(sparse_matrix* self,real* b,real* x);
/*
 * Conjugate Grade solver
 * self - matrix
 * b - right-part vector
 * x0 - first approximation of the solution
 * max_iter - pointer to maximum number of iterations, MAX_ITER if zero;
 * will contain a number of iterations passed
 * tolerance - pointer to desired tolerance value, TOLERANCE if zero;
 * will contain norm of the residual at the end of iteration
 * x - output vector
 */
static void sparse_matrix_solve_cg(sparse_matrix* self,
                                   real* b,
                                   real* x0,
                                   int* max_iter,
                                   real* tolerance,
                                   real* x);

/*
 * Preconditioned Conjugate Grade solver
 * Preconditioner in form of the ILU decomposition
 * self - matrix
 * b - right-part vector
 * x0 - first approximation of the solution
 * max_iter - pointer to maximum number of iterations, MAX_ITER if zero;
 * will contain a number of iterations passed
 * tolerance - pointer to desired tolerance value, TOLERANCE if zero;
 * will contain norm of the residual at the end of iteration
 * x - output vector
 */
static void sparse_matrix_solve_pcg(sparse_matrix* self,
                                   real* b,
                                   real* x0,
                                   int* max_iter,
                                   real* tolerance,
                                   real* x);


/*
 * Create ILU decomposition of the sparse matrix in skyline format
 * lu_diag - ILU decomposition diagonal
 * lu_lowertr - lower triangle of the ILU decomposition
 * lu_uppertr - upper triangle of the ILU decomposition
 */
static void sparse_matrix_skyline_ilu(sparse_matrix_skyline* self,
                                      real *lu_diag,
                                      real *lu_lowertr,
                                      real *lu_uppertr);


#ifdef DUMP_DATA
static void sparse_matrix_dump(sparse_matrix* self);
static void sparse_matrix_skyline_dump(sparse_matrix_skyline* self);
#endif




/*************************************************************/
/* General functions                                         */

/*
 * A function which will be called in case of error to
 * clear all memory occupied by internal structures.
 * Will clear global variable global_solver in a proper way
 * by calling free_fea_solver.
 * Also shall clear other allocated resources
 */
void application_done(void);


/*
 * Parse command line parameters and return input file name
 * into the filename variable
 */
int parse_cmdargs(int argc, char **argv,char **filename);

/*
 * Real main function with input filename as a parameter
 */
int do_main(char* filename);

/*
 * Test function to prove what all matrix manipulations are correct
 * returns FALSE if fail
 */
BOOL do_tests();

/*
 * Solver function which shall be called
 * when all data read to an appropriate structures
 */
void solve( fea_task *task,
            fea_solution_params *fea_params,
            nodes_array *nodes,
            elements_array *elements,
            prescribed_boundary_array *presc_boundary);

#ifdef DUMP_DATA
/* Dump input data to check if parser works correctly */
void dump_input_data( fea_task *task,
                      fea_solution_params *fea_params,
                      nodes_array *nodes,
                      elements_array *elements,
                      prescribed_boundary_array *presc_boundary);
#endif

/*************************************************************/
/* Functions for fea_solver structure                        */

/*
 * Fills solver with pointers to functions and pointers to arrays
 * of gauss nodes for particular element type.
 * This function is used on a construction phase
 */
static void solver_create_element_params(fea_solver* self);

/* Allocates memory and construct elements database for solver */
static void solver_create_element_database(fea_solver* self);
/* Destructor for the element database */
static void solver_free_element_database(fea_solver* self);

/*
 * Constructs material tensor of 4th rank
 * by given deformation gradient graddef into the output array
 * ctensor
 */
static void solver_ctensor(fea_solver* self,
                           real (*graddef)[MAX_DOF],
                           real (*ctensor)[MAX_DOF][MAX_DOF][MAX_DOF]);

/*
 * Returns a particular component of a node with local index 'node'
 * in element with index 'element' for the d.o.f. 'dof'
 */
static real solver_node_dof(fea_solver* self,
                            int element,
                            int node,
                            int dof)
{
  return self->nodes->nodes[self->elements->elements[element][node]][dof];
}


/*
 * Constructor for the shape functions gradients array
 * element - index of the element to calculate in
 * gauss - index of gauss node
 */
static shape_gradients* solver_new_shape_gradients(fea_solver* self,
                                                   int element,
                                                   int gauss);
/* Destructor for the shape gradients array */
static void solver_free_shape_gradients(fea_solver* self,shape_gradients* grads);

/* Create and distribute local stiffness matrix for the element */
static void solver_local_stiffness(fea_solver* self,int element);

/* Fill greate global forces vector */
static void solver_create_forces_bc(fea_solver* self);

/* Apply BC in form of prescribed displacements */
static void solver_apply_prescribed_bc(fea_solver* self);

/* Apply BC in form of prescribed displacements to a single specified
 * global d.o.f.
 * This function is called from solver_apply_prescribed_bc
 */
static void solver_apply_single_bc(fea_solver* self, int index, real value);

/*************************************************************/
/* Auxulary functions                                        */

/* Calculates the determinant of the matrix 3x3 */
real det3x3(real (*matrix3x3)[3]);

/*
 * Calculates in-place inverse of the matrix 3x3
 * Returns FALSE if the matrix is ill-formed
 * det shall store a determinant of the matrix
 */
BOOL inv3x3(real (*matrix3x3)[3], real* det);

int main(int argc, char **argv)
{
  char* filename = 0;
  int result = 0;

  /* Perform tests before start */
  if (!do_tests())
  {
    printf("Error! Tests failed!\n");
    return 1;
  }
  
  do
  {
    if ( TRUE == (result = parse_cmdargs(argc, argv,&filename)))
      break;
    if ( TRUE == (result = do_main(filename)))
      break;
  } while(0);

  return result;
}

int do_main(char* filename)
{
  /* initialize variables */
  int result = 0;
  fea_task *task = (fea_task *)0;
  fea_solution_params *fea_params = (fea_solution_params*)0;
  nodes_array *nodes = (nodes_array*)0;
  elements_array *elements = (elements_array*)0;
  prescribed_boundary_array *presc_boundary = (prescribed_boundary_array*)0;

  /* Set the application exit handler */
  /* atexit(application_done); */
  
  /* load geometry and solution details */
  if(!initial_data_load(filename,
                        &task,
                        &fea_params,
                        &nodes,
                        &elements,
                        &presc_boundary))
  {
    printf("Error. Unable to load %s.\n",filename);
    result = 1;
  }
  
  /* solve task */
  solve(task, fea_params, nodes, elements, presc_boundary);
  
  return result;
}

void solve( fea_task *task,
            fea_solution_params *fea_params,
            nodes_array *nodes,
            elements_array *elements,
            prescribed_boundary_array *presc_boundary)
{
  /* initialize variables */
  fea_solver *solver = (fea_solver*)0;
  int i;
  
#ifdef DUMP_DATA
  int j;
  FILE* f;
  /* Dump all data in debug version */
  dump_input_data(task,fea_params,nodes,elements,presc_boundary);
#endif
  /* Prepare solver instance */
  solver = new_fea_solver(task,
                          fea_params,
                          nodes,
                          elements,
                          presc_boundary);
  /* backup solver to the global_solver for the case of emergency exit */
  global_solver = solver;

  /* Create elements database */
  solver_create_element_database(solver);

  /* Create a global stiffness matrix */
  
  /* loop by elements - create local stiffnesses and redistribute them
   * in the global stiffness matrix */
  for ( i = 0; i < solver->elements->elements_count; ++ i)
    solver_local_stiffness(solver,i);
  /* sort column indicies in global matrix */
  sparse_matrix_reorder(&solver->global_mtx);

#ifdef DUMP_DATA
  if ((f = fopen("row_indexes.txt","w+")))
  {
    for ( i = 0; i < solver->global_mtx.rows_count; ++ i)
    {
      for ( j = 0; j <= solver->global_mtx.rows[i].last_index; ++ j)
        fprintf(f,"%d ",solver->global_mtx.rows[i].indexes[j]+1);
      fprintf(f,"\n");
    }
    fclose(f);
  }
#endif  
  
  /* fill the external forces vector */
  solver_create_forces_bc(solver);
  /* apply prescribed boundary conditions */
  solver_apply_prescribed_bc(solver);

#ifdef DUMP_DATA
  sparse_matrix_dump(&solver->global_mtx);
#endif
  /* solve global equation system */
  sparse_matrix_solve(&solver->global_mtx,
                   solver->global_forces_vct,
                   solver->global_solution_vct);
  for (i = 0; i < solver->global_mtx.rows_count; ++ i)
    printf("%f\n",solver->global_solution_vct[i]);
  
  free_fea_solver(solver);
  global_solver = (fea_solver*)0;
}


int parse_cmdargs(int argc, char **argv,char **filename)
{
  if (argc < 2)
  {
    printf("Usage: fea_solve input_data.xml\n");
    return 1;
  }
  *filename = argv[1];
  return 0;
}

#ifdef DUMP_DATA
void dump_input_data( fea_task *task,
                      fea_solution_params *fea_params,
                      nodes_array *nodes,
                      elements_array *elements,
                      prescribed_boundary_array *presc_boundary)
{
  int i,j;
  FILE *f;
  if ((f = fopen("input_data.txt","w+")))
  {
    fprintf(f,"nodes\n");
    for ( i = 0; i < nodes->nodes_count; ++ i)
    {
      for ( j = 0; j < MAX_DOF; ++ j)
        fprintf(f,"%f ", nodes->nodes[i][j]);
      fprintf(f,"\n");
    }
    fprintf(f,"elements\n");
    for ( i = 0; i < elements->elements_count; ++ i)
    {
      for ( j = 0; j < fea_params->nodes_per_element; ++ j)
        fprintf(f,"%d ", elements->elements[i][j]);
      fprintf(f,"\n");
    }
    fprintf(f,"boundary\n");
    for ( i = 0; i < presc_boundary->prescribed_nodes_count; ++ i)
    {
      fprintf(f,"%d %f %f %f %d\n",presc_boundary->prescribed_nodes[i].node_number,
              presc_boundary->prescribed_nodes[i].values[0],
              presc_boundary->prescribed_nodes[i].values[1],
              presc_boundary->prescribed_nodes[i].values[2],
              presc_boundary->prescribed_nodes[i].type);
    }
    fclose(f);
  }
}
#endif

void application_done(void)
{
  /* TODO: add additional finalization routines here */
  if (global_solver)
  {
    free_fea_solver(global_solver);
  }
#ifdef DMALLOC
  dmalloc_shutdown();
#endif
  
}

void init_sparse_matrix(sparse_matrix* mtx,
                    int rows,
                    int cols,
                    int bandwidth)
{
  int i;
  if (mtx)
  {
    mtx->rows_count = rows;
    mtx->cols_count = cols;
    mtx->rows = (indexed_array*)malloc(sizeof(indexed_array)*rows);
    /* create rows with fixed bandwidth */
    for (i = 0; i < rows; ++ i)
    {
      mtx->rows[i].width = bandwidth;
      mtx->rows[i].last_index = -1;
      mtx->rows[i].indexes = (int*)malloc(sizeof(int)*bandwidth);
      mtx->rows[i].values = (real*)malloc(sizeof(real)*bandwidth);
      memset(mtx->rows[i].indexes,0,sizeof(int)*bandwidth);
      memset(mtx->rows[i].values,0,sizeof(real)*bandwidth);
    }
  }
}


void free_sparse_matrix(sparse_matrix* mtx)
{
  int i;
  if (mtx)
  {
    for (i = 0; i < mtx->rows_count; ++ i)
    {
      free(mtx->rows[i].indexes);
      free(mtx->rows[i].values);
    }
    free(mtx->rows);
    mtx->rows = (indexed_array*)0;
    mtx->cols_count = 0;
    mtx->rows_count = 0;
  }
}

void init_sparse_matrix_skyline(sparse_matrix_skyline* self,sparse_matrix* mtx)
{
  /*
   * Construct CSLR matrix from the sparse_matrix
   * with symmetric portrait
   */
  int i,j,k,iptr,l_count,u_count,column;
  real* pvalue = 0;

  self->rows_count = mtx->rows_count;
  self->cols_count = mtx->cols_count;
  /*
   * get an information about number of nonzero elements
   */
  self->nonzeros = 0;
  for (i = 0; i < mtx->rows_count; ++ i)
    self->nonzeros += mtx->rows[i].last_index + 1;
  
  /* calculate number of upper-triangle elements */
  l_count = 0;
  u_count = 0;
  for (i = 0; i < mtx->rows_count; ++ i)
    for (j = 0; j <= mtx->rows[i].last_index; ++ j)
      if ( mtx->rows[i].indexes[j] > i)
        u_count ++;
      else if (mtx->rows[i].indexes[j] < i)
        l_count ++;
  /*
   * check if the number of upper triangle nonzero elements
   * is the same as number of lower triangle nonzero elements
   */
  assert(l_count == u_count);
  self->triangle_nonzeros_count = l_count;
  
  /* allocate memory for arrays */
  self->diag = (real*)malloc(sizeof(real)*mtx->rows_count);
  self->lower_triangle = l_count ? (real*)malloc(sizeof(real)*l_count) : 0;
  self->upper_triangle = u_count ? (real*)malloc(sizeof(real)*u_count) : 0;
  self->jptr = l_count ? (int*)malloc(sizeof(int)*l_count)  : 0;
  self->iptr = (int*)malloc(sizeof(int)*(mtx->rows_count+1));

  /* fill diagonal */
  for (i = 0; i < mtx->rows_count; ++ i)
  {
    pvalue = sparse_matrix_element(mtx,i,i);
    self->diag[i] = pvalue ? *pvalue : 0;
  }
  /* now fill arrays with proper values */
  u_count = 0,l_count = 0;
  for (i = 0; i < mtx->rows_count; ++ i)
  {
    iptr = -1;
    self->iptr[i] = 0;
    for (j = 0; j <= mtx->rows[i].last_index; ++ j)
    {
      if ( mtx->rows[i].indexes[j] < i)
      {
        /*
         * set a flag what we found the first nonzero element in
         * current row in lower triangle
         */
        if (iptr == -1)
          iptr  = l_count;
        /* fill lower triangle values */
        column = mtx->rows[i].indexes[j];
        self->jptr[l_count] = column;
        self->lower_triangle[l_count] = mtx->rows[i].values[j];
        /* fill upper triangle values - column-wise */
        for ( k = 0; k <= mtx->rows[column].last_index; ++ k)
          if (mtx->rows[column].indexes[k] == i)
          {
            self->upper_triangle[l_count] =
              mtx->rows[column].values[k];
            break;
          }
        l_count ++;
      }
    }
    self->iptr[i] = iptr == -1 ? l_count : iptr;
  }
  /* finalize iptr array */
  self->iptr[i] = self->triangle_nonzeros_count;
}

void free_sparse_matrix_skyline(sparse_matrix_skyline* self)
{
  if (self)
  {
    self->rows_count = 0;
    self->cols_count = 0;
    self->nonzeros = 0;
    self->triangle_nonzeros_count = 0;
    free(self->diag);
    free(self->lower_triangle);
    free(self->upper_triangle);
    free(self->jptr);
    free(self->iptr);
  }
}


real* sparse_matrix_element(sparse_matrix* self,int i, int j)
{
  int index;
  /* check for matrix and if i,j are proper indicies */
  if (self && 
      (i >= 0 && i < self->rows_count ) &&
      (j >= 0 && j < self->cols_count ))
  {
    /* loop by nonzero columns in row i */
    for (index = 0; index <= self->rows[i].last_index; ++ index)
      if (self->rows[i].indexes[index] == j)
        return &self->rows[i].values[index];
  }
  return (real*)0;
}

void sparse_matrix_element_add(sparse_matrix* self,int i, int j, real value)
{
  int index,new_width;
  int* indexes = (int*)0;
  real* values = (real*)0;
  /* check for matrix and if i,j are proper indicies */
  if (self && 
      (i >= 0 && i < self->rows_count ) &&
      (j >= 0 && j < self->cols_count ))
  {
    /* loop by nonzero columns in row i */
    for (index = 0; index <= self->rows[i].last_index; ++ index)
      if (self->rows[i].indexes[index] == j)
      {
        /* nonzerod element found, add to it */
        self->rows[i].values[index] += value;
        return;
      }
    /* needed to add a new element to the row */
    
    /*
     * check if bandwidth is not exceed and reallocate memory
     * if necessary
     */
    if (self->rows[i].last_index == self->rows[i].width - 1)
    {
      new_width = self->rows[i].width*2;
      indexes = (int*)realloc(self->rows[i].indexes,new_width*sizeof(int));
      assert(indexes);
      self->rows[i].indexes = indexes;
      values = (real*)realloc(self->rows[i].values,new_width*sizeof(real));
      assert(values);
      self->rows[i].values = values;
      self->rows[i].width = new_width;
    }
    /* add an element to the row */
    self->rows[i].last_index++;
    self->rows[i].values[self->rows[i].last_index] = value;
    self->rows[i].indexes[self->rows[i].last_index] = j;
  }
}

/* Swap 2 elements of the indexed array */
void indexed_array_swap(indexed_array* self,int i, int j)
{
  int tmp_idx;
  real tmp_val;
  tmp_idx = self->indexes[i];
  self->indexes[i] = self->indexes[j];
  self->indexes[j] = tmp_idx;
  tmp_val = self->values[i];
  self->values[i] = self->values[j];
  self->values[j] = tmp_val;
}

void indexed_array_sort(indexed_array* self, int l, int r)
{
  /*
   * Quick sort procedure for indexed(compressed) arrays
   * for example rows for CRS sparse matrix or columns for CSC
   * sparse matrix
   */
  int pivot,i;
  int tmp_idx;

  /* boundary checks */
  if (l < r)
  {
    if ( r - l == 1)
    {
      if (self->indexes[l] > self->indexes[r])
        indexed_array_swap(self,r,l);
      return;
    }
    /* choose the pivoting element */
    pivot = (int)((r+l)/2.);
    /* in-place partition procedure - move all elements
     * lower than pivoting to the left, greater to the right */
    tmp_idx  = self->indexes[pivot];
    indexed_array_swap(self,pivot,r);
    pivot = l;
    for ( i = l; i < r; ++ i)
    {
      if (self->indexes[i] <= tmp_idx )
      {
        indexed_array_swap(self,i,pivot);
        pivot++;
      }
    }
    indexed_array_swap(self,r,pivot);
    /* repeat procedure for the left and right parts of an array */
    indexed_array_sort(self,l,pivot-1);
    indexed_array_sort(self,pivot+1,r);
  }
}


void sparse_matrix_reorder(sparse_matrix* self)
{
  int i;
  
  for (i = 0; i < self->rows_count; ++ i)
    indexed_array_sort(&self->rows[i],0,self->rows[i].last_index);
}

void sparse_matrix_mv(sparse_matrix* self,real* x, real* y)
{
  int i,j;
  for ( i = 0; i < self->rows_count; ++ i)
  {
    y[i] = 0;
    for ( j = 0; j <= self->rows[i].last_index; ++ j)
      y[i] += self->rows[i].values[j]*x[self->rows[i].indexes[j]];
  }
}

void sparse_matrix_solve(sparse_matrix* self,real* b,real* x)
{
  real tolerance = 1e-15;
  int max_iter = 20000;
  sparse_matrix_solve_pcg(self,b,b,&max_iter,&tolerance,x);
}

void sparse_matrix_solve_cg(sparse_matrix* self,
                                real* b,
                                real* x0,
                                int* max_iter,
                                real* tolerance,
                                real* x)
{
  /* Conjugate Gradient Algorithm */
  /*
   * Taken from the book:
   * Saad Y. Iterative methods for sparse linear systems (2ed., 2000)
   * page 178
   */
   
  /* variables */
  int i,j;
  real alpha, beta,a1,a2;
  real residn = 0;
  int size = sizeof(real)*self->rows_count;
  int msize = self->rows_count;
  real max_iterations = max_iter ? *max_iter : MAX_ITER;
  real tol = tolerance ? *tolerance : TOLERANCE;
  real* r;              /* residual */
  real* p;              /* search direction */
  real* temp;
  
  /* allocate memory for vectors */
  r = malloc(size);
  p = malloc(size);
  temp = malloc(size);
  /* clear vectors */
  memset(r,0,size);
  memset(p,0,size);
  memset(temp,0,size);

  /* x = x_0 */
  for ( i = 0; i < msize; ++ i)
    x[i] = x0[i];

  /* r_0 = b - A*x_0 */
  sparse_matrix_mv(self,b,r);
  for ( i = 0; i < msize; ++ i)
    r[i] = b[i] - r[i];

  /* p_0 = r_0 */
  memcpy(p,r,size);
  
  /* CG loop */
  for ( j = 0; j < max_iterations; j ++ )
  {
    /* temp = A*p_j */
    sparse_matrix_mv(self,p,temp);
    /* compute (r_j,r_j) and (A*p_j,p_j) */
    a1 = 0; a2 = 0;
    for (i = 0; i < msize; ++ i)
    {
      a1 += r[i]*r[i]; /* (r_j,r_j) */
      a2 += p[i]*temp[i];      /* (A*p_j,p_j) */
    }

    /*            (r_j,r_j) 
     * alpha_j = -----------
     *           (A*p_j,p_j)
     */                     
    alpha = a1/a2;              
                                
    /* x_{j+1} = x_j+alpha_j*p_j */
    for (i = 0; i < msize; ++ i)
      x[i] += alpha*p[i];
    
    /* r_{j+1} = r_j-alpha_j*A*p_j */
    for (i = 0; i < msize; ++ i)
      r[i] -= alpha*temp[i]; 

    /* check for convergence */
    residn = fabs(r[0]);
    for (i = 1; i < msize; ++ i )
      if (fabs(r[i]) > residn) residn = fabs(r[i]);
    if (residn < tol )
      break;

    /* compute (r_{j+1},r_{j+1}) */
    a2 = 0;
    for (i = 0; i < msize; ++ i)
      a2 += r[i]*r[i];

    /* b_j = (r_{j+1},r_{j+1})/(r_j,r_j) */
    beta = a2/a1;
    
    /* d_{j+1} = r_{j+1} + beta_j*d_j */
    for (i = 0; i < msize; ++ i)
      p[i] = r[i] + beta*p[i];
  }
  *max_iter = j;
  *tolerance = residn;
  
  free(r);
  free(p);
  free(temp);
}

void sparse_matrix_solve_pcg(sparse_matrix* self,
                             real* b,
                             real* x0,
                             int* max_iter,
                             real* tolerance,
                             real* x)
{
  /* variables */
  int i,j;
  real alpha, beta,a1,a2;
  real residn = 0;
  int size = sizeof(real)*self->rows_count;
  int msize = self->rows_count;
  real max_iterations = max_iter ? *max_iter : MAX_ITER;
  real tol = tolerance ? *tolerance : TOLERANCE;
  
  /* skyline form of the initial matrix */
  sparse_matrix_skyline A;
  real *lu_diag;
  real *lu_lowertr;
  real *lu_uppertr;

  real* r;              /* residual */
  real* p;              /* search direction */
  real* temp;
  
  /* allocate memory for vectors */
  r = malloc(size);
  p = malloc(size);
  temp = malloc(size);
  /* initialize skyline matrix for ILU decomposition */
  init_sparse_matrix_skyline(&A,self);
  
  lu_diag = malloc(sizeof(real)*msize);
  lu_lowertr = malloc(sizeof(real)*A.triangle_nonzeros_count);
  lu_uppertr = malloc(sizeof(real)*A.triangle_nonzeros_count);

  
  /* clear vectors */
  memset(r,0,size);
  memset(p,0,size);
  memset(temp,0,size);

  /* sparse_matrix_skyline_ilu(&A,lu_diag,lu_lowertr,lu_uppertr); */
  
  /* x = x_0 */
  for ( i = 0; i < msize; ++ i)
    x[i] = x0[i];

  /* r_0 = b - A*x_0 */
  sparse_matrix_mv(self,b,r);
  for ( i = 0; i < msize; ++ i)
    r[i] = b[i] - r[i];

  /* p_0 = r_0 */
  memcpy(p,r,size);
  
  /* CG loop */
  for ( j = 0; j < max_iterations; j ++ )
  {
    /* temp = A*p_j */
    sparse_matrix_mv(self,p,temp);
    /* compute (r_j,r_j) and (A*p_j,p_j) */
    a1 = 0; a2 = 0;
    for (i = 0; i < msize; ++ i)
    {
      a1 += r[i]*r[i]; /* (r_j,r_j) */
      a2 += p[i]*temp[i];      /* (A*p_j,p_j) */
    }

    /*            (r_j,r_j) 
     * alpha_j = -----------
     *           (A*p_j,p_j)
     */                     
    alpha = a1/a2;              
                                
    /* x_{j+1} = x_j+alpha_j*p_j */
    for (i = 0; i < msize; ++ i)
      x[i] += alpha*p[i];
    
    /* r_{j+1} = r_j-alpha_j*A*p_j */
    for (i = 0; i < msize; ++ i)
      r[i] -= alpha*temp[i]; 

    /* check for convergence */
    residn = fabs(r[0]);
    for (i = 1; i < msize; ++ i )
      if (fabs(r[i]) > residn) residn = fabs(r[i]);
    if (residn < tol )
      break;

    /* compute (r_{j+1},r_{j+1}) */
    a2 = 0;
    for (i = 0; i < msize; ++ i)
      a2 += r[i]*r[i];

    /* b_j = (r_{j+1},r_{j+1})/(r_j,r_j) */
    beta = a2/a1;
    
    /* d_{j+1} = r_{j+1} + beta_j*d_j */
    for (i = 0; i < msize; ++ i)
      p[i] = r[i] + beta*p[i];
  }
  *max_iter = j;
  *tolerance = residn;

  free(lu_diag);
  free(lu_lowertr);
  free(lu_uppertr);
  
  free(r);
  free(p);
  free(temp);

  free_sparse_matrix_skyline(&A);
}

void sparse_matrix_skyline_ilu(sparse_matrix_skyline* self,
                               real *lu_diag,
                               real *lu_lowertr,
                               real *lu_uppertr)
{
  int i,j,k,l,q;
  real sum;
  /* clear arrays before construction of the ILU decomposition */
  memset(lu_diag,0,sizeof(real)*self->rows_count);
  memset(lu_lowertr,0,sizeof(real)*self->triangle_nonzeros_count);
  memset(lu_uppertr,0,sizeof(real)*self->triangle_nonzeros_count);

  for (k = 0; k < self->rows_count; ++ k)
  {
    for ( j = self->iptr[k]; j < self->iptr[k+1]; ++ j)
    {
      sum = 0;
      for ( i = self->iptr[k]; i < j; ++ i)
        for ( l = self->iptr[j]; l < self->iptr[j+1]; ++ l)
        {
          if ( self->jptr[i] == self->jptr[l] )
            sum += lu_lowertr[i]*lu_uppertr[l];
        }
      lu_lowertr[j] =
        (self->lower_triangle[j] - sum)/lu_diag[self->jptr[j]];
    }

    /*
     * U_{kk} = A_{kk} -
     * \sum\limits_{i=1}^{k-1} L_{ki}U_{ik}
     */
    sum = 0;
    for ( i = self->iptr[k]; i < self->iptr[k+1]; ++ i)
      sum += lu_lowertr[i]*lu_uppertr[i];
    lu_diag[k] = self->diag[k] - sum;

    for (j = k; j < self->rows_count; ++ j)
    {
      for ( q = self->iptr[j]; q < self->iptr[j+1]; ++ q)
        if (k == self->jptr[q])
        {
          /*
           * U_{kj} = A_{kj} -
           * \sum\limits_{i=1}^{k-1}L_{ki}U_{ij}
           */
          sum = 0;
          /*
           * i = iptr[k]:iptr[k+1]-1 are coordinates of the
           * k-th row in lower matrix array (lu_lowertr)
           * l = iptr[j]:iptr[j+1]-1 are coordinates of the
           * j-th column in upper matrix array (lu_uppertr)
           */
          for ( i = self->iptr[k]; i < self->iptr[k+1]; ++ i)
            for ( l = self->iptr[j]; l < self->iptr[j+1]; ++ l)
            {
              /* if row and column indicies are the same */
              if ( self->jptr[i] == self->jptr[l] )
                sum += lu_lowertr[i]*lu_uppertr[l];
            }
          lu_uppertr[q] = self->upper_triangle[q] - sum;
          printf("%.2f,U(%d,%d)=%.2f\t",sum,k,q,lu_uppertr[q]);
          break;
        }
    }
    printf("\n");
  }
  
}



#ifdef DUMP_DATA
void sparse_matrix_dump(sparse_matrix* self)
{
  FILE* f;
  int i,j;
  real *pvalue,value;
  if ((f = fopen("mywidths.txt","w+")))
  {
    for ( i = 0; i < self->rows_count; ++ i)
      fprintf(f,"%d: %d\n",i+1,self->rows[i].last_index + 1);
    fclose(f);
  }
  if( (f = fopen("rows.txt","w+")))
  {
    for ( i = 0; i < self->rows_count; ++ i)
    {
      for ( j = 0; j <= self->rows[i].last_index; ++ j)
        fprintf(f,"%d ",self->rows[i].indexes[j] + 1);
      fprintf(f,"\n");
      for ( j = 0; j <= self->rows[i].last_index; ++ j)
        fprintf(f,"%f ",self->rows[i].values[j]);
      fprintf(f,"\n\n");
    }
    fclose(f);
  }

  if ((f = fopen("global_matrix_c.txt","w+")))
  {
    for (i = 0; i < self->rows_count; ++ i)
    {
      for (j = 0; j < self->rows_count; ++ j)
      {
        pvalue = sparse_matrix_element(self,i,j);
        value = pvalue ? *pvalue : 0;
        fprintf(f,"%.5f ",value);
      }
      fprintf(f,"\n");
    }
    fclose(f);
  }
}



void sparse_matrix_skyline_dump(sparse_matrix_skyline* self)
{
  int i;
  
  printf("adiag = [");
  for ( i = 0; i < self->rows_count; ++ i )
    printf("%.1f ",self->diag[i]);
  printf("]\n");

  printf("altr = [");
  for ( i = 0; i < self->triangle_nonzeros_count; ++ i )
    printf("%.1f ",self->lower_triangle[i]);
  printf("]\n");

  printf("autr = [");
  for ( i = 0; i < self->triangle_nonzeros_count; ++ i )
    printf("%.1f ",self->upper_triangle[i]);
  printf("]\n");
  
  printf("jptr = [");
  for ( i = 0; i < self->triangle_nonzeros_count; ++ i )
    printf("%d ",self->jptr[i]+1);
  printf("]\n");

  printf("iptr = [");
  for ( i = 0; i < self->triangle_nonzeros_count-1; ++ i )
    printf("%d ",self->iptr[i]+1);
  printf("]\n");
}
#endif



fea_solver* new_fea_solver(fea_task *task,
                           fea_solution_params *fea_params,
                           nodes_array *nodes,
                           elements_array *elements,
                           prescribed_boundary_array *prs_boundary)
{
  int msize,bandwidth;
  /* Allocate structure */
  fea_solver* solver = malloc(sizeof(fea_solver));
  /* Copy pointers to the solver structure */
  solver->task = task;
  solver->fea_params = fea_params;
  solver->nodes = nodes;
  solver->elements = elements;
  solver->presc_boundary = prs_boundary;

  solver->elements_db.gauss_nodes = (gauss_node**)0;
  solver_create_element_params(solver);

  /* allocate resources initialize global stiffness matrix */
  /* global matrix size */
  msize = nodes->nodes_count*solver->task->dof;
  /* approximate bandwidth of a global matrix
   * usually sqrt(msize)*2*/
  bandwidth = (int)sqrt(msize)*2;
  init_sparse_matrix(&solver->global_mtx,msize,msize,bandwidth);
  /* allocate memory for global forces and solution vectors */
  solver->global_forces_vct = (real*)malloc(sizeof(real)*msize);
  solver->global_solution_vct = (real*)malloc(sizeof(real)*msize);
  memset(solver->global_forces_vct,0,sizeof(real)*msize);
  memset(solver->global_solution_vct,0,sizeof(real)*msize);
  return solver;
}


void free_fea_solver(fea_solver* solver)
{
  /* deallocate resources */
  solver_free_element_database(solver);
  free_fea_task(solver->task);
  free_fea_solution_params(solver->fea_params);
  free_nodes_array(solver->nodes);
  free_elements_array(solver->elements);
  free_prescribed_boundary_array(solver->presc_boundary);
  free_sparse_matrix(&solver->global_mtx);
  free(solver->global_forces_vct);
  free(solver->global_solution_vct);
  free(solver);
}

/*
 * Creates a particular gauss node for the element
 * with index element_index and gauss node number gauss_node_index
 */
static gauss_node *solver_new_gauss_node(fea_solver* self,
                                         int gauss_node_index)
{
  gauss_node* node = (gauss_node*)0;
  int i,j;
  real r,s,t;
  /* Check for array bounds*/
  if (gauss_node_index >= 0 &&
      gauss_node_index < self->fea_params->gauss_nodes_count)
  {
    node = malloc(sizeof(gauss_node));
    /* set the weight for this gauss node */
    node->weight = self->elements_db.gauss_nodes_data[gauss_node_index][0];
    /* set shape function values and their derivatives for this node */
    node->forms = malloc(sizeof(real)*(self->fea_params->nodes_per_element));
    node->dforms = malloc(sizeof(real*)*(self->task->dof));
    for ( i = 0; i < self->task->dof; ++ i)
      node->dforms[i] = malloc(sizeof(real)*(self->fea_params->nodes_per_element));
    for ( i = 0; i < self->fea_params->nodes_per_element; ++ i)
    {
      r = self->elements_db.gauss_nodes_data[gauss_node_index][1];
      s = self->elements_db.gauss_nodes_data[gauss_node_index][2];
      t = self->elements_db.gauss_nodes_data[gauss_node_index][3];
      node->forms[i] = self->shape(i,r,s,t);
      for ( j = 0; j < self->task->dof; ++ j)
        node->dforms[j][i] = self->dshape(i,j,r,s,t);
    }

  }
  return node;
}

/* Deallocate gauss node */
static void solver_free_gauss_node(fea_solver *self,
                                   gauss_node *node)
{
  int i;
  if (node)
  {
    /* clear forms and dforms arrays */
    free(node->forms);
    for ( i = 0; i < self->task->dof; ++ i)
      free(node->dforms[i]);
    free(node->dforms);
    /* free the node itself */
    free(node);
  }
}
  

void solver_create_element_database(fea_solver* self)
{
  int gauss;
  int gauss_count = self->fea_params->gauss_nodes_count;
  /* Create database only if not created yet */
  if (!self->elements_db.gauss_nodes)
  {
    /* allocate memory for gauss nodes array */
    self->elements_db.gauss_nodes =
      malloc(sizeof(gauss_node*)*gauss_count);
    for (gauss = 0; gauss < gauss_count; ++ gauss)
      self->elements_db.gauss_nodes[gauss] =
        solver_new_gauss_node(self,gauss);
    
  }
}

void solver_free_element_database(fea_solver* solver)
{
  int gauss;
  if (solver->elements_db.gauss_nodes)
  {
    for (gauss = 0; gauss < solver->fea_params->gauss_nodes_count; ++gauss)
      solver_free_gauss_node(solver,solver->elements_db.gauss_nodes[gauss]);
    
    free(solver->elements_db.gauss_nodes);
  }
}

static void solver_create_element_params_tetrahedra10(fea_solver* solver);

/*
 * Creates particular element-dependent data in fea_solver
 * All new element types shall be added here 
 */
void solver_create_element_params(fea_solver* solver)
{
  switch (solver->task->ele_type)
  {
  case TETRAHEDRA10:
    solver_create_element_params_tetrahedra10(solver);
    break;
  default:
    /* TODO: add error handling here */
    printf("Error: unknown element type");
    exit(1);
  };
  
}
#ifdef DUMP_DATA
static void solver_dump_shape_gradients(fea_solver* self,
                                        shape_gradients* grads,
                                        int element,
                                        int gauss,
                                        real (*J)[MAX_DOF])
{
  int i,j;
  FILE* f;
  if ((f = fopen("gradients.txt","w+")))
  {
    fprintf(f,"\nElement %d:\n",element);
    for ( j = 0; j < self->fea_params->nodes_per_element; ++ j)
      fprintf(f,"%d ",self->elements->elements[element][j]);
    fprintf(f,"\nNodes:\n");
    for ( j = 0; j < self->fea_params->nodes_per_element; ++ j)
    {
      for ( i = 0; i < MAX_DOF; ++ i)
        fprintf(f,"%f ",self->nodes->nodes[self->elements->elements[element][j]][i]);
      fprintf(f,"\n");
    }
    fprintf(f,"\nGauss node %d:\n",gauss);
    for ( i = 0; i < MAX_DOF; ++ i)
      fprintf(f,"%f ",self->elements_db.gauss_nodes_data[gauss][i+1]);
    fprintf(f,"\n\nDeterminant of Jacobi matrix(det(J): %f\n",grads->detJ);
    
    fprintf(f,"\nInverse Jacobi matrix(J^-1):\n");
    for ( i = 0; i < MAX_DOF; ++ i)
    {
      for ( j = 0; j < MAX_DOF; ++ j)
        fprintf(f,"%f ",J[i][j]);
      fprintf(f,"\n");
    }
    fprintf(f,"\nMatrix of gradients:\n");
    for ( i = 0; i < MAX_DOF; ++ i)
    {
      for ( j = 0; j < self->fea_params->nodes_per_element; ++ j)
        fprintf(f,"%.5f ",grads->grad[i][j]);
      fprintf(f,"\n");
    }
    fclose(f);
  }
}
#endif

shape_gradients* solver_new_shape_gradients(fea_solver* self,
                                                   int element,
                                                   int gauss)
{
  int i,j,k;
  int row_size;
  real detJ;
  /* J is a Jacobi matrix of transformation btw local and global */
  /* coordinate systems */
  real J[MAX_DOF][MAX_DOF];
  shape_gradients* grads = (shape_gradients*)0;

  /* Fill an array using Bonet & Wood 7.6(a,b) p.198, 1st edition */
  /* also see Zienkiewitz v1, 6th edition, p.146-147 */

  for (i = 0; i < MAX_DOF; ++ i)
    memset(&J[i],0,sizeof(real)*MAX_DOF);
  /* First, fill the Jacobi matrix (3x3) */
  /* I = 1..n, n - number of nodes per element
   * x_I, y_I, z_I - nodal coordinates for the element
   *           dN_1(r,s,t)            dN_n(r,s,t)     
   * J(1,1) =  ---------- * x_1 + ... ---------- * x_n
   *               dr                      dr
   *
   *           dN_1(r,s,t)            dN_n(r,s,t)     
   * J(1,2) =  ---------- * y_1 + ... ---------- * y_n
   *               dr                      dr
   *
   *           dN_1(r,s,t)            dN_n(r,s,t)     
   * J(2,1) =  ---------- * x_1 + ... ---------- * x_n
   *               ds                      ds
   * ...
   */
  for (i = 0; i < MAX_DOF; ++ i)
    for (j = 0; j < MAX_DOF; ++ j)
    {
      for (k = 0; k < self->fea_params->nodes_per_element; ++ k)
        J[i][j] += self->elements_db.gauss_nodes[gauss]->dforms[i][k]* \
          solver_node_dof(self,element,k,j);
    }
  if (inv3x3(J,&detJ))                /* inverse exists */
  {
    /* Allocate memory for shape gradients */
    grads = (shape_gradients*)malloc(sizeof(shape_gradients));
    grads->grad = (real**)malloc(sizeof(real*)*(self->task->dof));
    row_size = sizeof(real)*(self->fea_params->nodes_per_element);
    for (i = 0; i < self->task->dof; ++ i)
    {
      grads->grad[i] = (real*)malloc(row_size);
      memset(grads->grad[i],0,row_size);
    }
    /* Store determinant of the Jacobi matrix */
    grads->detJ = detJ;
    
    /* [ dN/dx ]           [ dN/dr ] */
    /* [ dN/dy ]  = J^-1 * [ dN/ds ] */
    /* [ dN/dz ]           [ dN/dt ] */
    for ( i = 0; i < MAX_DOF; ++ i)
      for ( j = 0; j < self->fea_params->nodes_per_element; ++ j)
        for ( k = 0; k < MAX_DOF; ++ k)
          grads->grad[i][j] += J[i][k]* \
            self->elements_db.gauss_nodes[gauss]->dforms[k][j];
#ifdef DUMP_DATA
    /* Dump results */
    solver_dump_shape_gradients(self,grads,element,gauss,J);
#endif
  }

  return grads;
}

/* Destructor for the shape gradients array */
void solver_free_shape_gradients(fea_solver* self,shape_gradients* grads)
{
  int i;
  /* for (i = 0; i < self->fea_params->nodes_per_element; ++ i */
  for (i = 0; i < self->task->dof; ++ i)
    free(grads->grad[i]);
  free(grads->grad);
  grads->grad = (real**)0;
  free(grads);
}

#ifdef DUMP_DATA
void solver_dump_local_stiffness(fea_solver* self,real **stiff,int el)
{
  int i,j;
  FILE* f;
  char fname[50];
  int size = self->fea_params->nodes_per_element*self->task->dof;
  sprintf(fname,"elements/K%d.txt",el);
  if ((f = fopen(fname,"w+")))
  {
    for ( i = 0; i < size; ++ i)
    {
      for ( j = 0; j < size; ++ j)
        fprintf(f,"%.5f ",stiff[i][j]);
      fprintf(f,"\n");
    }
    fclose(f);
  }
}
#endif

#ifdef DUMP_DATA
void matrix_tensor_mapping(int I, int* i, int* j)
{
  switch (I)
  {
  case 0:
    *i = 0; *j = 0;
    break;
  case 1:
    *i = 1; *j = 1;
    break;
  case 2:
    *i = 2; *j = 2;
    break;
  case 3:
    *i = 0; *j = 1;
    break;
  case 4:
    *i = 1; *j = 2;
    break;
  case 5:
    *i = 0; *j = 2;
  }
}

void dump_ctensor_as_matrix(real (*ctensor)[MAX_DOF][MAX_DOF][MAX_DOF])
{
  int I,J;
  int i = 0, j = 0, k = 0, l = 0;
  FILE* f;
  if ((f = fopen("ctensor.txt","w+")))
  {
    fprintf(f,"\nConstitutive matrix:\n"); 
    for (I = 0; I < 6; ++ I)
    {
      matrix_tensor_mapping(I,&i,&j);
      for (J = 0; J < 6;++ J)
      {
        matrix_tensor_mapping(J,&k,&l);
        fprintf(f,"%f ",ctensor[i][j][k][l]);
      }
      fprintf(f,"\n");
    }
    fclose(f);
  }
}
#endif


void solver_local_stiffness(fea_solver* self,int element)
{
  /* matrix of gradients of shape functions */
  shape_gradients* grads = (shape_gradients*)0;
  int gauss,a,b,i,j,k,l,I,J,globalI,globalJ;
  real sum;
  /* size of a local stiffness matrix */
  int size;
  /* number of nodes per element */
  int nelem;
  /* current number of d.o.f */
  int dof;
  /* local stiffness matrix */
  real **stiff = (real**)0;
  /* deformation gradient */
  real graddef[MAX_DOF][MAX_DOF];
  /* C tensor depending on material model */
  real ctens[MAX_DOF][MAX_DOF][MAX_DOF][MAX_DOF];
  
  /* allocate memory for a local stiffness matrix */
  size = self->fea_params->nodes_per_element*self->task->dof;
  stiff = (real**)malloc(sizeof(real*)*size);
  for (i = 0; i < size; ++ i)
  {
    stiff[i] = (real*)malloc(sizeof(real)*size);
    memset(stiff[i],0,sizeof(real)*size);
  }
  
  /* obtain a C tensor */
  solver_ctensor(self,graddef,ctens);
  
#ifdef DUMP_DATA  
  dump_ctensor_as_matrix(ctens);
#endif
  
  dof = self->task->dof;
  nelem = self->fea_params->nodes_per_element;
  
  /* loop by gauss nodes - numerical integration */
  for (gauss = 0; gauss < self->fea_params->gauss_nodes_count ; ++ gauss)
  {
    grads = solver_new_shape_gradients(self,element,gauss);
    if (grads)
    {
      /* Construct components of stiffness matrix in
       * indical form using Bonet & Wood 7.35 p.207, 1st edition */
      
      /* loop for nodes */
      for ( a = 0; a < nelem; ++ a)
        for (b = 0; b < nelem; ++ b)
        {
          /* loop for d.o.f in a stiffness matrix block [K_{ab}]ij, 3x3 */
          for (i = 0; i < dof; ++ i)
            for (j = 0; j < dof; ++ j)
            {
              /* indicies in a local stiffness matrix */
              I = a*dof + i;
              J = b*dof + j;
              sum = 0.0;
              /* sum of particular derivatives and components of C tensor */
              for (k = 0; k < dof; ++ k)
                for (l = 0; l < dof; ++ l)
                  sum+=grads->grad[k][a]*ctens[i][k][j][l]*grads->grad[l][b];
              /*
               * multiply by volume of an element = det(J)
               * where divider 6 or 2 or others already accounted in
               * weights of gauss nodes
               */
              sum *= fabs(grads->detJ);
              /* ... and weight of the gauss nodes for  */
              sum *= self->elements_db.gauss_nodes[gauss]->weight;
              /* append to the local stiffness */
              stiff[I][J] += sum;
              /* finally distribute to the global matrix */
              globalI = self->elements->elements[element][a]*dof + i;
              globalJ = self->elements->elements[element][b]*dof + j;
              sparse_matrix_element_add(&self->global_mtx,
                                     globalI,
                                     globalJ,
                                     sum);
            }
        }
    }
    solver_free_shape_gradients(self,grads);
  }
  
#ifdef DUMP_DATA
  solver_dump_local_stiffness(self,stiff,element);
#endif
  
  /* clear local stiffness */
  for ( i = 0; i < size; ++ i )
    free(stiff[i]);
  free(stiff);
}



void solver_ctensor(fea_solver* self,
                    real (*graddef)[MAX_DOF],
                    real (*ctensor)[MAX_DOF][MAX_DOF][MAX_DOF])
{
  int i,j,k,l;
  real lambda,mu;
  lambda = self->task->model.parameters[0];
  mu = self->task->model.parameters[1];
  for ( i = 0; i < MAX_DOF; ++ i )
    for ( j = 0; j < MAX_DOF; ++ j )
      for ( k = 0; k < MAX_DOF; ++ k )
        for ( l = 0; l < MAX_DOF; ++ l )
          ctensor[i][j][k][l] = 
            lambda * DELTA (i, j) * DELTA (k, l)    \
            + mu * DELTA (i, k) * DELTA (j, l)    \
            + mu * DELTA (i, l) * DELTA (j, k);
}

void solver_create_forces_bc(fea_solver* self)
{
  /* TODO: implement this */
  /* solver->global_forces_vct */
}

void solver_apply_prescribed_bc(fea_solver* self)
{
  int i;
  int type,index,offset,node_number;
  real presc[3];
  for ( i =0; i < self->presc_boundary->prescribed_nodes_count; ++ i)
  {
    node_number = self->presc_boundary->prescribed_nodes[i].node_number;
    memcpy(presc,
           self->presc_boundary->prescribed_nodes[i].values,
           sizeof(real)*self->task->dof);
    type = self->presc_boundary->prescribed_nodes[i].type;

    /* set the index offset depending on condition type */
    if ( type == PRESCRIBEDX || type == PRESCRIBEDXY || 
         type == PRESCRIBEDXZ || type == PRESCRIBEDXYZ )
    {
      offset = 0;
      index = node_number*self->task->dof+offset;
      solver_apply_single_bc(self, index, presc[offset]);
    }
    if ( type == PRESCRIBEDY || type == PRESCRIBEDXY || 
         type == PRESCRIBEDYZ || type == PRESCRIBEDXYZ )
    {
      offset = 1;
      index = node_number*self->task->dof+offset;
      solver_apply_single_bc(self, index, presc[offset]);
    }
    if ( type == PRESCRIBEDZ || type == PRESCRIBEDXZ || 
         type == PRESCRIBEDYZ || type == PRESCRIBEDXYZ )
    {
      offset = 2;
      index = node_number*self->task->dof+offset;
      solver_apply_single_bc(self, index, presc[offset]);
    }
  }

  /* for (i = 0; i < self->global_mtx.rows_count; ++ i) */
  /*   printf("%f\n",self->global_forces_vct[i]); */
}

void solver_apply_single_bc(fea_solver* self, int index, real presc)
{
  real *pvalue,*pvalue1,value;
  int size = self->global_mtx.rows_count;
  int j;
  pvalue = sparse_matrix_element(&self->global_mtx,index,index);
  /* global matrix always shall have
   * nonzero diagonal elements */
  assert(pvalue);
  pvalue1 = pvalue;
  value = *pvalue;
  
  for (j = 0; j < size; ++ j)
  {
    pvalue = sparse_matrix_element(&self->global_mtx,j,index);
    if (pvalue)
    {
      self->global_forces_vct[j] = self->global_forces_vct[j] -
        *pvalue*presc;
      *pvalue = 0;
    }
    pvalue = sparse_matrix_element(&self->global_mtx,index,j);
    if (pvalue)
      *pvalue = 0;
  }
  
  *pvalue1 = value;
  self->global_forces_vct[index] = value*presc;
}


void solver_apply_single_bc2(fea_solver* self, int index, real presc)
{
  real *pvalue,new_value;
  real Alpha = 1e8;

  pvalue = sparse_matrix_element(&self->global_mtx,index,index);
  assert(pvalue);             /* global matrix always shall have
                               * nonzero diagonal elements */
  new_value = *pvalue*Alpha;
  *pvalue = new_value;
  self->global_forces_vct[index] = new_value*presc;
}



/* function for calculation value of shape function for 10-noded
 * tetrahedra 
 * by given element el, node number i,local coordinates r,s,t,  
 * where r,s,t from [0;1] 
 * all functions are taken from the book: 
 * "The Finite Element Method for 3D Thermomechanical Applications"
 * by - Guido Dhond p.72
 */
real tetrahedra10_isoform(int i,real r,real s,real t)
{
  switch(i)
  {
  case 0: return (2*(1-r-s-t)-1)*(1-r-s-t);
  case 1: return (2*r-1)*r;
  case 2: return (2*s-1)*s;
  case 3: return (2*t-1)*t;
  case 4: return 4*r*(1-r-s-t);
  case 5: return 4*r*s;
  case 6: return 4*s*(1-r-s-t);
  case 7: return 4*t*(1-r-s-t);
  case 8: return 4*r*t;
  case 9: return 4*s*t;
  }
  return 0;
}

/*
 * Element TETRAHEDRA10, isoparametric shape function derivative
 * with respece to the 1st variable r
 * i - node number
 */ 
real tetrahedra10_df_dr(int i,real r,real s,real t)
{
  switch(i)
  {
  case 0: return 4*t+4*s+4*r-3;
  case 1: return 4*r-1;
  case 2: return 0;
  case 3: return 0;
  case 4: return -4*t-4*s-8*r+4;
  case 5: return 4*s;
  case 6: return -4*s;
  case 7: return -4*t;
  case 8: return 4*t;
  case 9: return 0;
  }
  return 0;
}

/*
 * Element TETRAHEDRA10, isoparametric shape function derivative
 * with respece to the 2nd variable s
 * i - node number
 */ 
real tetrahedra10_df_ds(int i, real r, real s, real t)
{
  switch(i)
  {
  case 0: return 4*t+4*s+4*r-3;
  case 1: return 0;
  case 2: return 4*s-1;
  case 3: return 0;
  case 4: return -4*r;
  case 5: return 4*r;
  case 6: return -4*t-8*s-4*r+4;
  case 7: return -4*t;
  case 8: return 0;
  case 9: return 4*t;
  }
  return 0;
}

/*
 * Element TETRAHEDRA10, isoparametric shape function derivative
 * with respece to the 3rd variable t
 * i - node number
 */ 
real tetrahedra10_df_dt(int i, real r, real s, real t)
{
  switch(i)
  {
  case 0: return 4*t+4*s+4*r-3;
  case 1: return 0;
  case 2: return 0;
  case 3: return 4*t-1;
  case 4: return -4*r;
  case 5: return 0;
  case 6: return -4*s;
  case 7: return -8*t-4*s-4*r+4;
  case 8: return 4*r;
  case 9: return 4*s;
  }
  return 0;
}

/*
 * function for calculation derivatives of shape 
 * function of 10noded tetrahedra element
 * with respect to local coordinate system
 * shape - number of node(and corresponding shape function)
 * dof - degree of freedom, dof = 1 is r, dof = 2 is s, dof = 3 is t
 * r,s,t is [0;1] - local coordinates
 */
real tetrahedra10_disoform(int shape,int dof,real r,real s,real t)
{
  switch(dof)
  {
  case 0: return tetrahedra10_df_dr(shape,r,s,t);
  case 1: return tetrahedra10_df_ds(shape,r,s,t);
  case 2: return tetrahedra10_df_dt(shape,r,s,t);
  }
  return 0;
}


void solver_create_element_params_tetrahedra10(fea_solver* solver)
{
  solver->shape = tetrahedra10_isoform;
  solver->dshape = tetrahedra10_disoform;
  switch (solver->fea_params->gauss_nodes_count)
  {
  case 4:
    solver->elements_db.gauss_nodes_data = gauss_nodes4_tetr10;
    break;
  case 5:
    solver->elements_db.gauss_nodes_data = gauss_nodes5_tetr10;
    break;
  }
}


static fea_task* new_fea_task()
{
  /* allocate memory */
  fea_task *task = (fea_task *)malloc(sizeof(fea_task));
  /* set default values */
  task->desired_tolerance = 1e-8;
  task->dof = 3;
  task->ele_type = TETRAHEDRA10;
  task->linesearch_max = 0;
  task->arclength_max = 0;
  task->load_increments_count = 0;
  task->type = CARTESIAN3D;
  task->modified_newton = TRUE;
  task->model.model = MODEL_A5;
  task->model.parameters_count = 2;
  task->model.parameters[0] = 100;
  task->model.parameters[1] = 100;
  return task;
}

static void free_fea_task(fea_task* task)
{
  free(task);
}

/* Initializes fea solution params with default values */
static fea_solution_params* new_fea_solution_params()
{
  /* allocate memory */
  fea_solution_params *fea_params = (fea_solution_params *)
    malloc(sizeof(fea_solution_params));
  /* set default values */
  fea_params->gauss_nodes_count = 5;
  fea_params->nodes_per_element = 10;
  return fea_params;
}

/* clear fea solution params */
static void free_fea_solution_params(fea_solution_params* params)
{
  free(params);
}

/* Initialize nodes array but not initialize particular arrays  */
static nodes_array* new_nodes_array()
{
  /* allocate memory */
  nodes_array *nodes = (nodes_array*)malloc(sizeof(nodes_array));
  /* set zero values */
  nodes->nodes = (real**)0;
  nodes->nodes_count = 0;
  return nodes;
}

/* carefully deallocate nodes array */
static void free_nodes_array(nodes_array* nodes)
{
  int counter = 0;
  if (nodes)
  {
    if (nodes->nodes_count && nodes->nodes)
    {
      for (; counter < nodes->nodes_count; ++ counter)
        free(nodes->nodes[counter]);
      free(nodes->nodes);
    }
    free(nodes);
  }
}


/* Initialize elements array but not initialize particular elements */
static elements_array* new_elements_array()
{
  /* allocate memory */
  elements_array *elements = (elements_array*)malloc(sizeof(elements_array));
  /* set zero values */
  elements->elements = (int**)0;
  elements->elements_count = 0;
  return elements;
}

static void free_elements_array(elements_array *elements)
{
  if(elements)
  {
    int counter = 0;
    if (elements->elements_count && elements->elements)
    {
      for (; counter < elements->elements_count; ++ counter)
        free(elements->elements[counter]);
      free(elements->elements);
    }
    free(elements);
  }
}

/* Initialize boundary nodes array but not initialize particular nodes */
static prescribed_boundary_array* new_prescribed_boundary_array()
{
  /* allocate memory */
  prescribed_boundary_array *presc_boundary = (prescribed_boundary_array*)
    malloc(sizeof(prescribed_boundary_array));
  /* set zero values */
  presc_boundary->prescribed_nodes = (prescibed_boundary_node*)0;
  presc_boundary->prescribed_nodes_count = 0;
  return presc_boundary;
}

static void free_prescribed_boundary_array(prescribed_boundary_array* presc)
{
  if (presc)
  {
    if (presc->prescribed_nodes_count && presc->prescribed_nodes)
    {
      free(presc->prescribed_nodes);
    }
    free(presc);
  }
}

real det3x3(real (*m)[3])
{
  real result;
  result = m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1]) - 
    m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0]) + 
    m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
  return result;
}

BOOL inv3x3(real (*m)[3],real* det)
{
  real m00,m01,m02,m10,m11,m12,m20,m21,m22;
  *det = det3x3(m);
  if (EQL(*det,0.0))
    return FALSE;
	/* calculate components */
	/* first row */
  m00 = (m[1][1]*m[2][2]-m[1][2]*m[2][1])/(*det);
	m01 = (m[0][2]*m[2][1]-m[0][1]*m[2][2])/(*det);
	m02 = (m[0][1]*m[1][2]-m[0][2]*m[1][1])/(*det);
	/* second row */
	m10 = (m[1][2]*m[2][0]-m[1][0]*m[2][2])/(*det);
	m11 = (m[0][0]*m[2][2]-m[0][2]*m[2][0])/(*det);
	m12 = (m[0][2]*m[1][0]-m[0][0]*m[1][2])/(*det);
	/* third row */
	m20 = (m[1][0]*m[2][1]-m[1][1]*m[2][0])/(*det);
	m21 = (m[0][1]*m[2][0]-m[0][0]*m[2][1])/(*det);
	m22 = (m[0][0]*m[1][1]-m[0][1]*m[1][0])/(*det);
  
  /* perform in-place substitution of the result */
	m[0][0] = m00; 	m[0][1] = m01; 	m[0][2] = m02;
	m[1][0] = m10; 	m[1][1] = m11; 	m[1][2] = m12;
	m[2][0] = m20;	m[2][1] = m21;	m[2][2] = m22;
  
  return TRUE;
}


/* Case-insensitive string comparsion procedure */
int istrcmp(s1,s2)
     const char *s1, *s2;
{
  /* case insensitive comparison */
  int d;
  for (;;) {
#ifdef ASCII_CTYPE
    if (!isascii(*s1) || !isascii(*s2))
      d = *s1 - *s2;
    else
#endif
      d = (tolower((unsigned char) *s1) - tolower((unsigned char)*s2));
    if ( d != 0 || *s1 == '\0' || *s2 == '\0' )
      return d;
    ++s1;
    ++s2;
  }
  /*NOTREACHED*/
}



#ifdef USE_EXPAT

#define INDEX_STACK_SIZE 5

/*
 * Stack for storing element or nodes indexes or sizes in xml representation
 * i.e. top level of the stack - number of nodes, a level below - current
 * node index, one more level below could be dof index for the current node
 * 
 */
typedef struct index_stack_tag {
  int storage[INDEX_STACK_SIZE];
  int level;
} index_stack; 

/* Stack operating functions */
/* Initialization */
void index_stack_init(index_stack* stack)
{
  memset(&stack->storage,0,sizeof(stack->storage));
  /* stack->level = -1 means no elements in stack */
  stack->level = -1;
}
/* Take a stack head element, or return FALSE if an empty */
BOOL index_stack_pop(index_stack* stack, int* value)
{
  if (stack->level == -1)
    return FALSE;
  *value = stack->storage[stack->level--];
  return TRUE;
}
/* Push element to the stack */
void index_stack_push(index_stack* stack, int value)
{
  if (stack->level == sizeof(stack->storage)/sizeof(int))
  {
    stack->level = 0;
    stack->storage[0] = value;
  }
  else
  {
    stack->storage[++stack->level] = value;
  }
}


/* All known XML tags */
typedef enum xml_format_tags_enum {
  UNKNOWN_TAG,
  TASK,
  MODEL,
  MODEL_PARAMETERS,
  SOLUTION,
  ELEMENT_TYPE,
  LINE_SEARCH,
  ARC_LENGTH,
  INPUT_DATA,
  GEOMETRY,
  NODES,
  NODE,
  ELEMENTS,
  ELEMENT,
  BOUNDARY_CONDITIONS,
  PRESCRIBED_DISPLACEMENTS,
  PRESC_NODE
} xml_format_tags;

/* Convert particular string to the XML tag enum */
static xml_format_tags tagname_to_enum(const XML_Char* name)
{
  if (!istrcmp(name,"TASK")) return TASK;
  if (!istrcmp(name,"MODEL")) return MODEL;
  if (!istrcmp(name,"MODEL-PARAMETERS")) return MODEL_PARAMETERS;
  if (!istrcmp(name,"SOLUTION")) return SOLUTION;
  if (!istrcmp(name,"ELEMENT-TYPE")) return ELEMENT_TYPE;
  if (!istrcmp(name,"LINE-SEARCH")) return LINE_SEARCH;
  if (!istrcmp(name,"ARC-LENGTH")) return ARC_LENGTH;
  if (!istrcmp(name,"INPUT-DATA")) return INPUT_DATA;
  if (!istrcmp(name,"GEOMETRY")) return GEOMETRY;
  if (!istrcmp(name,"NODES")) return NODES;
  if (!istrcmp(name,"NODE")) return NODE;
  if (!istrcmp(name,"ELEMENTS")) return ELEMENTS;
  if (!istrcmp(name,"ELEMENT")) return ELEMENT;
  if (!istrcmp(name,"BOUNDARY-CONDITIONS")) return BOUNDARY_CONDITIONS;
  if(!istrcmp(name,"PRESCRIBED-DISPLACEMENTS"))return PRESCRIBED_DISPLACEMENTS;
  if (!istrcmp(name,"PRESC-NODE")) return PRESC_NODE;
  return UNKNOWN_TAG;
}

/* An input data structure used in parser */
typedef struct parse_data_tag {
  fea_task *task;
  fea_solution_params *fea_params;
  nodes_array *nodes;
  elements_array *elements;
  prescribed_boundary_array *presc_boundary;
  index_stack stack;
  xml_format_tags parent_tag;
  char* current_text;
  int current_size;
} parse_data;


/*
 * Remove leading and trailing whitespaces from the string,
 * allocating null-terminated string as a result
 */
char *trim_whitespaces(const char* string,size_t size)
{
  const char* end = string+size;
  char* result = (char*)0;
  int not_ws_start = 0;
  int not_ws_end = 0;
  const char* ptr = string;
  /* find starting non-whitespace character */
  while( isspace(*ptr++) && size-- ) not_ws_start++;
  if (size != 0 || not_ws_start == 0)
  {
    ptr--;
    /* find trailing non-whitespace character */
    while(isspace(*--end) && end != ptr) not_ws_end++;
    size = end-ptr+1;
    result = (char*)malloc(size+1);
    memcpy(result,ptr,size);
    result[size] = '\0';
  }
  return result;
}

/*
 * Functions called from expat_start/end_tag_handler
 * when the tag is known
 */
void process_begin_tag(parse_data* data, int tag,const XML_Char **atts);
void process_end_tag(parse_data* data, int tag);

/* Expat start tag handler */
void expat_start_tag_handler(void *userData,
                      const XML_Char *name,
                      const XML_Char **atts)
{
  parse_data* data = (parse_data*)userData;
  int tag = tagname_to_enum(name);
  if(tag != UNKNOWN_TAG)
    process_begin_tag(data,tag,atts);
}

/* Expat End tag handler */
void expat_end_tag_handler(void *userData,
                   const XML_Char *name)
{
  parse_data* data = (parse_data*)userData;
  int tag = tagname_to_enum(name);

  if (tag != UNKNOWN_TAG)
    process_end_tag(data,tag);
  /* clear tag text data at tag close */
  if (data->current_text)
    free(data->current_text);
  data->current_text = (char*)0;
  data->current_size = 0;
}

/*
 * Expat handler for the text between tags
 * Since this function could be called several times for the current tag,
 * it is necessary to store text somewhere. We use parse_data->current_text
 * pointer and parse_data->current_size for these purposes
 */
void expat_text_handler(void *userData,
                        const XML_Char *s,
                        int len)
{
  parse_data* data = (parse_data*)userData;
  char *ptr;
  if (len)
  {
    if (!data->current_text)    /* allocate memory for the text in tag */
    {
      data->current_text = (char*)malloc(len);
      ptr = data->current_text;
    }
    else                        /* reallocate/widen memory alread allocated */
    {
      data->current_text = (char*)realloc(data->current_text,
                                          data->current_size+len);
      ptr = data->current_text;
      ptr += data->current_size;  
    }
    /* append text to the end of allocated/reallocad buffer */
    /* and increase size variable */
    memcpy(ptr,s,len);
    data->current_size += len;
  }
}


static BOOL expat_data_load(char *filename,
                            fea_task **task,
                            fea_solution_params **fea_params,
                            nodes_array **nodes,
                            elements_array **elements,
                            prescribed_boundary_array **presc_boundary)
{
  BOOL result = FALSE;
  FILE* xml_document_file;
  XML_Parser parser;
  size_t file_size = 0;
  size_t read_bytes = 0;
  parse_data parse;
  enum XML_Status status;
  char *file_contents = (char*)0;

  /* Try to open file */
  if (!(xml_document_file = fopen(filename,"rt")))
  {
    printf("Error, could not open file %s\n",filename);
    return FALSE;
  }
  /* Determine file size */
  if (fseek(xml_document_file,0,SEEK_END))
  {
    printf("Error reading file %s\n",filename);
    return FALSE;
  }
  file_size = ftell(xml_document_file);
  /* rewind to the begin of file */
  fseek(xml_document_file,0,SEEK_SET);

  /* Create parser */
  parser = XML_ParserCreate(NULL);
  /* Set handlers */
  XML_SetElementHandler(parser, &expat_start_tag_handler, &expat_end_tag_handler);
  XML_SetCharacterDataHandler(parser,expat_text_handler);

  /* initialize data */
  parse.current_text = (char*)0;
  parse.current_size = 0;

  /* read whole file */
  file_contents = (char*)malloc(file_size);
  read_bytes = fread(file_contents,1,file_size,xml_document_file);
  if (errno)
  {
    free(file_contents);
    return FALSE;
  }
  fclose(xml_document_file);

  /* allocate parse data */
  parse.task = new_fea_task();
  parse.fea_params = new_fea_solution_params();
  parse.nodes = new_nodes_array();
  parse.elements = new_elements_array();
  parse.presc_boundary = new_prescribed_boundary_array();
  index_stack_init(&parse.stack);
  parse.current_size = 0;
  parse.current_text = (char*)0;
  /* set user data */
  XML_SetUserData(parser,&parse);

  /* call parser */
  status = XML_Parse(parser,file_contents,(int)read_bytes,1);
  free(file_contents);
  XML_ParserFree(parser);
  
  *task = parse.task;
  *fea_params = parse.fea_params;
  *nodes = parse.nodes;
  *elements = parse.elements;
  *presc_boundary = parse.presc_boundary;

  
  result = TRUE;
  return result;
}


/* test if an attribute name is what expected(attribute_name)
 * and increase pointer to the next attribute if yes*/
BOOL check_attribute(const char* attribute_name, const XML_Char ***atts)
{
  BOOL result = FALSE;
  if(!istrcmp(**atts,attribute_name))
  {
    (*atts)++;
    result = TRUE;
  }
  return result;
}

/* model tag handler */
void process_model_type(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  for (; *atts; atts++ )
  {
    if (check_attribute("name",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      if (!istrcmp(text,"A5")) 
      {
        data->task->model.model = MODEL_A5;
        data->task->model.parameters_count = 2;
      }
      else
      {
        printf("unknown model type %s\n",text);
      }
      if(text)
        free(text);
    }
  }
}

/* model-parameters tag handler */
void process_model_params(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  int count = 0;
  for (; *atts && count < data->task->model.parameters_count; atts++ )
  {
    atts++;
    text = trim_whitespaces(*atts,strlen(*atts));
    data->task->model.parameters[count] = atof(text);
    if(text)
      free(text);
    count++;
  }
}

/* solution tag handler */
void process_solution(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  for (; *atts; atts++ )
  {
    if (check_attribute("modified-newton",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->task->modified_newton = (!istrcmp(text,"yes") || !istrcmp(text,"true"))? TRUE: FALSE;
      if (text)
        free(text);
    }
    else if (check_attribute("task-type",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      if (!istrcmp(text,"CARTESIAN3D"))
        data->task->type = CARTESIAN3D;
      if (text)
        free(text);
    }
    else if (check_attribute("load-increments-count",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->task->load_increments_count = atoi(text);
      if (text)
        free(text);
    }
    else if (check_attribute("desired-tolerance",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->task->desired_tolerance = atof(text);
      if (text)
        free(text);
    }
  }
  data->parent_tag = SOLUTION;
}

/* element-type tag handler */
void process_element_type(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  for (; *atts; atts++ )
  {
    if (check_attribute("name",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      if (!istrcmp(text,"TETRAHEDRA10"))
        data->task->ele_type = TETRAHEDRA10;
      if (text)
        free(text);
    }
    else if (check_attribute("nodes-count",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->fea_params->nodes_per_element = atoi(text);
      if (text)
        free(text);
    }
    else if (check_attribute("nodes-count",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->fea_params->nodes_per_element = atoi(text);
      if (text)
        free(text);
    }
    else if (check_attribute("gauss-nodes-count",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->fea_params->gauss_nodes_count = atoi(text);
      if (text)
        free(text);
    }
  }
}

/* line-search tag handler */
void process_line_search(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  for (; *atts; atts++ )
  {
    if (check_attribute("max",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->task->linesearch_max = atoi(text);
      if (text)
        free(text);
    }
  }
}

/* arc-length tag handler */
void process_arc_length(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  for (; *atts; atts++ )
  {
    if (check_attribute("max",&atts))
    {
      text = trim_whitespaces(*atts,strlen(*atts));
      data->task->arclength_max = atoi(text);
      if(text)
        free(text);
    }
  }
}

/* nodes tag handler */
void process_nodes(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  int i = 0;
  /* set parameters only when 'nodes' tag is a child of the 'geometry' tag */
  if ( data->parent_tag == GEOMETRY )
  {
    for (; *atts; atts++ )
    {
      if (check_attribute("count",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        data->nodes->nodes_count = atoi(text);
        /* allocate storage for nodes */
        data->nodes->nodes =
          (real**)malloc(data->nodes->nodes_count*sizeof(real*));
        for (; i < data->nodes->nodes_count; ++ i)
          data->nodes->nodes[i] = (real*)malloc(MAX_DOF*sizeof(real));
        if (text)
          free(text);
      }
    }
    /* set parent tag to 'nodes' to recoginze an appropriate 'node' tag */
    data->parent_tag = NODES;
  }
}

/* node tag handler */
void process_node(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  real dofs[MAX_DOF];
  int id = -1;
  if ( data->parent_tag == NODES )
  {
    for (; *atts; atts++ )
    {
      if (check_attribute("id",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        id = atoi(text);
        if (text) free(text);
      }
      else if (check_attribute("x",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        dofs[0] = atof(text);
        if (text) free(text);
      }
      else if (check_attribute("y",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        dofs[1] = atof(text);
        if (text) free(text);
      }
      else if (check_attribute("z",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        dofs[2] = atof(text);
        if (text) free(text);
      }
    }
    if (id != -1)
      memcpy(data->nodes->nodes[id],dofs,sizeof(dofs));
  }
}

/* elements tag handler */
void process_elements(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  int i = 0;
  /* set parameters only when 'nodes' tag is a child of the 'geometry' tag */
  if ( data->parent_tag == GEOMETRY )
  {
    for (; *atts; atts++ )
    {
      if (check_attribute("count",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        data->elements->elements_count = atoi(text);
        /* allocate storage for elements */
        data->elements->elements = 
          (int**)malloc(data->elements->elements_count*sizeof(int*));
        for (; i < data->elements->elements_count; ++ i)
          data->elements->elements[i] =
            (int*)malloc(data->fea_params->nodes_per_element*sizeof(int));
        if (text) free(text);
      }
    }
    /* set parent tag to 'ELEMENTS' to recoginze an appropriate 'ELEMENT' tag */
    data->parent_tag = ELEMENTS;
  }
}

/* take the node id/position from the element attributes
 * like 'node1' or 'node10'
 * returns -1 in case of wrong attribute name
 * but not skip it in this case!
 */
int node_position_from_attr(const XML_Char ***atts)
{
  int result = -1;
  char* pos = strstr(**atts,"node");
  if (pos == **atts)
  {
    result = atoi(pos+strlen("node"))-1;
    (*atts)++;
  }
  return result;
}

/* element tag handler */
void process_element(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  int pos = -1;
  int element_size_bytes = data->fea_params->nodes_per_element*sizeof(int);
  int* element = (int*)malloc(element_size_bytes);
  int id = -1;
  if ( data->parent_tag == ELEMENTS )
  {
    for (; *atts; atts++ )
    {
      if (check_attribute("id",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        id = atoi(text);
        if (text) free(text);
      }
      if (-1 != (pos = node_position_from_attr(&atts)))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        element[pos] = atoi(text); 
        if (text) free(text);
      }
    }
    if (id != -1)
      memcpy(data->elements->elements[id],element,element_size_bytes);
  }
  free(element);
}


/* prescribed-displacements tag handler */
void process_prescribed_displacements(parse_data* data, const XML_Char **atts)
{
  char* text = (char*)0;
  int size;
  /* set parameters only when 'nodes' tag is a child of the 'geometry' tag */
  if ( data->parent_tag == BOUNDARY_CONDITIONS )
  {
    for (; *atts; atts++ )
    {
      if (check_attribute("count",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        data->presc_boundary->prescribed_nodes_count = atoi(text);
        size = data->presc_boundary->prescribed_nodes_count;
        /* allocate storage for prescribed nodes */
        data->presc_boundary->prescribed_nodes =  
          (prescibed_boundary_node*)malloc(size*sizeof(prescibed_boundary_node));
        if (text) free(text);
      }
    }
    /* set parent tag to 'nodes' to recoginze an appropriate 'node' tag */
    data->parent_tag = PRESCRIBED_DISPLACEMENTS;
  }
}

/* node tag handler */
void process_prescribed_node(parse_data* data, const XML_Char **atts)
{
  		/* <presc-node id="1" node-id="10" x="0" y="0" z="0" type="7"/> */
  char* text = (char*)0;
  prescibed_boundary_node node;
  int id = -1;
  if ( data->parent_tag == PRESCRIBED_DISPLACEMENTS )
  {
    for (; *atts; atts++ )
    {
      if (check_attribute("id",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        id = atoi(text);
        if (text) free(text);
      }
      else if (check_attribute("node-id",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        node.node_number = atoi(text);
        if (text) free(text);
      }
      else if (check_attribute("x",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        node.values[0] = atof(text);
        if (text) free(text);
      }
      else if (check_attribute("y",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        node.values[1] = atof(text);
        if (text) free(text);
      }
      else if (check_attribute("z",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        node.values[2] = atof(text);
        if (text) free(text);
      }
      else if (check_attribute("type",&atts))
      {
        text = trim_whitespaces(*atts,strlen(*atts));
        /* TODO: add proper conversion */
        node.type= (prescribed_boundary_type)atoi(text);
        if (text) free(text);
      }
    }
    if (id != -1)
      memcpy(&data->presc_boundary->prescribed_nodes[id],&node,sizeof(node));
  }
}

void process_begin_tag(parse_data* data, int tag,const XML_Char **atts)
{
  switch(tag)
  {
  case TASK:
    break;
  case MODEL:
    process_model_type(data,atts);
    break;
  case MODEL_PARAMETERS:
    process_model_params(data,atts);
    break;
  case SOLUTION:
    process_solution(data,atts);
    break;
  case ELEMENT_TYPE:
    process_element_type(data,atts);
    break;
  case LINE_SEARCH:
    process_line_search(data,atts);
    break;
  case ARC_LENGTH:
    process_arc_length(data,atts);
    break;
  case INPUT_DATA:
    data->parent_tag = INPUT_DATA;
    break;
  case GEOMETRY:
    data->parent_tag = GEOMETRY;
    break;
  case NODES:
    process_nodes(data,atts);
    break;
  case NODE:
    process_node(data,atts);
    break;
  case ELEMENTS:
    process_elements(data,atts);
    break;
  case ELEMENT:
    process_element(data,atts);
    break;
  case BOUNDARY_CONDITIONS:
    data->parent_tag = BOUNDARY_CONDITIONS;
    break;
  case PRESCRIBED_DISPLACEMENTS:
    process_prescribed_displacements(data,atts);
    data->parent_tag = PRESCRIBED_DISPLACEMENTS;
    break;
  case PRESC_NODE:
    process_prescribed_node(data,atts);
    break;
  default:
    break;
  };
}

void process_end_tag(parse_data* data, int tag)
{
  switch(tag)
  {
  case NODE:
    break;
  case ELEMENT:
    break;
  case PRESC_NODE:
    break;
  case MODEL:
  case SOLUTION:
  case INPUT_DATA:    
    data->parent_tag = TASK;
    break;
  case MODEL_PARAMETERS:
    data->parent_tag = MODEL;
    break;
  case ELEMENT_TYPE:
  case LINE_SEARCH:
  case ARC_LENGTH:
    data->parent_tag = SOLUTION;
    break;
  case GEOMETRY:
  case BOUNDARY_CONDITIONS:
    data->parent_tag = INPUT_DATA;
    break;
  case NODES:
  case ELEMENTS:
    data->parent_tag = GEOMETRY;
    break;
  case TASK:
  default:
    data->parent_tag = UNKNOWN_TAG;
    break;
  };
}


#endif



BOOL initial_data_load(char *filename,
                       fea_task **task,
                       fea_solution_params **fea_params,
                       nodes_array **nodes,
                       elements_array **elements,
                       prescribed_boundary_array **presc_boundary)
{
  BOOL result = FALSE;
#ifdef USE_EXPAT
  result = expat_data_load(filename,task,fea_params,nodes,elements,presc_boundary);
#endif
  return result;
}




BOOL do_tests()
{
  BOOL result = TRUE;
  sparse_matrix mtx,mtx2;
  real v[3],x[3];
  sparse_matrix_skyline m;
  real *lu_diag;
  real *lu_lowertr;
  real *lu_uppertr;
  int i;
  
  /* 1st test, matrix solver  */
  
  /*
   * | 1 0 -2 |   | 1 |   |-5 |
   * | 0 1  0 | x | 2 | = | 2 | 
   * |-2 0  5 |   | 3 |   |13 |
   */
   
  memset(x,0,3);
  v[0] = -5;
  v[1] = 2;
  v[2] = 13;
  init_sparse_matrix(&mtx,3,3,2);
  sparse_matrix_element_add(&mtx,0,2,-2);
  sparse_matrix_element_add(&mtx,0,0,1);

  sparse_matrix_element_add(&mtx,1,1,1);
  
  sparse_matrix_element_add(&mtx,2,2,5);
  sparse_matrix_element_add(&mtx,2,0,-2);


  sparse_matrix_reorder(&mtx);

  sparse_matrix_solve(&mtx,v,x);
  result = !( fabs(x[0]-1) > TOLERANCE ||
              fabs(x[1]-2) > TOLERANCE ||
              fabs(x[2]-3) > TOLERANCE);


  /* Sparse matrix from Balandin
   * 9  0  0  3  1  0  1
   * 0  11 2  1  0  0  2
   * 0  1  10 2  0  0  0
   * 2  1  2  9  1  0  0
   * 1  0  0  1  12 0  1
   * 0  0  0  0  0  8  0
   * 2  2  0  0  3  0  8
   */
   
  init_sparse_matrix(&mtx2,7,7,5);
#define MTX(m,i,j,v) sparse_matrix_element_add((m),(i),(j),(v));
  MTX(&mtx2,0,0,9);MTX(&mtx2,0,3,3);MTX(&mtx2,0,4,1);MTX(&mtx2,0,6,1);
  MTX(&mtx2,1,1,11);MTX(&mtx2,1,2,2);MTX(&mtx2,1,3,1);MTX(&mtx2,1,6,2);
  MTX(&mtx2,2,1,1);MTX(&mtx2,2,2,10);MTX(&mtx2,2,3,2);
  MTX(&mtx2,3,0,2);MTX(&mtx2,3,1,1);MTX(&mtx2,3,2,2);MTX(&mtx2,3,3,9);MTX(&mtx2,3,4,1);
  MTX(&mtx2,4,0,1);MTX(&mtx2,4,3,1);MTX(&mtx2,4,4,12);MTX(&mtx2,4,6,1);
  MTX(&mtx2,5,5,8);
  MTX(&mtx2,6,0,2);MTX(&mtx2,6,1,2);MTX(&mtx2,6,4,3);MTX(&mtx2,6,6,8);
#undef MTX

  sparse_matrix_reorder(&mtx2);
  
  init_sparse_matrix_skyline(&m,&mtx2);

#ifdef DUMP_DATA
  sparse_matrix_skyline_dump(&m);
#endif

  lu_diag = malloc(sizeof(real)*m.rows_count);
  lu_lowertr = malloc(sizeof(real)*m.triangle_nonzeros_count);
  lu_uppertr = malloc(sizeof(real)*m.triangle_nonzeros_count);

  sparse_matrix_skyline_ilu(&m,lu_diag,lu_lowertr,lu_uppertr);

  printf("lu_diag = [");
  for (i = 0; i <  m.rows_count; ++ i)
    printf("%f ",lu_diag[i]);
  printf("];\n");
  
  printf("lu_lowertr = [");
  for (i = 0; i <  m.triangle_nonzeros_count; ++ i)
    printf("%f ",lu_lowertr[i]);
  printf("];\n");
  
  printf("lu_uppertr = [");
  for (i = 0; i <  m.triangle_nonzeros_count; ++ i)
    printf("%f ",lu_uppertr[i]);
  printf("];\n");

  
  free(lu_diag);
  free(lu_lowertr);
  free(lu_uppertr);
  
  free_sparse_matrix(&mtx);
  free_sparse_matrix(&mtx2);
  free_sparse_matrix_skyline(&m);
  return FALSE;
  return result;
}
