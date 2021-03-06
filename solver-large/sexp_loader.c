/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <stdlib.h>
#include <limits.h>

/*
 * by some reason necessary to include this to avoid
 * collect2: ld terminated with signal 11 [Segmentation fault]
 * crash.
 * it is the case since errno is used
 */
#include <errno.h>

#include "sexp_loader.h"
#include "libsexp.h"

/* An input data structure used in parser */
typedef struct {
  fea_task *task;
  fea_solution_params *fea_params;
  nodes_array *nodes;
  elements_array *elements;
  presc_bnd_array *presc_boundary;
  char* current_text;
  int current_size;
} parse_data;


static void process_model(sexp_item* item, parse_data* data)
{
  /* first determine model type */
  sexp_item* value = sexp_item_attribute(item,"name");
  if (value)
  {
    if (sexp_item_is_symbol_like(value,"A5"))
    {
      data->task->model.model = MODEL_A5;
      data->task->model.parameters_count = 2;
    }
    else if (sexp_item_is_symbol_like(value,"COMPRESSIBLE_NEOHOOKEAN"))
    {
      data->task->model.model = MODEL_COMPRESSIBLE_NEOHOOKEAN;
      data->task->model.parameters_count = 2;
    }
    else
    {
      printf("unknown model type '%s'\n",sexp_item_symbol(value));
    }
  }
}

static void process_model_parameters(sexp_item* item, parse_data* data)
{
  sexp_item* value; 
  switch(data->task->model.model)
  {
  case MODEL_COMPRESSIBLE_NEOHOOKEAN:
  case MODEL_A5:
    value = sexp_item_attribute(item,"lambda");
    assert(value);
    data->task->model.parameters[0] = sexp_item_fnumber(value);
    value = sexp_item_attribute(item,"mu");
    assert(value);
    data->task->model.parameters[1] = sexp_item_fnumber(value);
    break;
  default:
    break;
  }
}

static void process_solution(sexp_item* item, parse_data* data)
{
  sexp_item* value;
  value = sexp_item_attribute(item,"desired-tolerance");
  assert(value);
  data->task->desired_tolerance = sexp_item_fnumber(value);
  value = sexp_item_attribute(item,"task-type");
  assert(value);
  if (sexp_item_is_symbol_like(value,"CARTESIAN3D"))
    data->task->type = CARTESIAN3D;
  value = sexp_item_attribute(item,"load-increments-count");
  assert(value);
  data->task->load_increments_count = sexp_item_inumber(value);
  value = sexp_item_attribute(item,"modified-newton");
  assert(value);
  data->task->modified_newton = FALSE;
  if (sexp_item_is_symbol_like(value,"YES") || sexp_item_is_symbol_like(value,"TRUE"))
    data->task->modified_newton = TRUE;
  value = sexp_item_attribute(item,"max-newton-count");
  data->task->max_newton_count = sexp_item_inumber(value);
}

static void process_slae_solver(sexp_item* item, parse_data* data)
{
  /* determine solver type */
  sexp_item* value = sexp_item_attribute(item,"type");
  /* set default values */
  data->task->solver_type = CG;
  data->task->solver_tolerance = MAX_ITERATIVE_TOLERANCE;
  data->task->solver_max_iter = MAX_ITERATIVE_ITERATIONS;
  if (value)
  {
    if (sexp_item_is_symbol_like(value,"CG"))
    {
      data->task->solver_type = CG;
      value = sexp_item_attribute(item,"tolerance");
      if (value)
        data->task->solver_tolerance = sexp_item_fnumber(value);
      value = sexp_item_attribute(item,"max-iterations");
      data->task->solver_max_iter = value ? sexp_item_inumber(value) :
        MAX_ITERATIVE_ITERATIONS;
    }
    else if (sexp_item_is_symbol_like(value,"PCG_ILU"))
    {
      data->task->solver_type = PCG_ILU;
      value = sexp_item_attribute(item,"tolerance");
      if (value)
        data->task->solver_tolerance = sexp_item_fnumber(value);
      value = sexp_item_attribute(item,"max-iterations");
      data->task->solver_max_iter = value ? sexp_item_inumber(value) :
        MAX_ITERATIVE_ITERATIONS;
    } 
    else if (sexp_item_is_symbol_like(value,"CHOLESKY"))
    {
      data->task->solver_type = CHOLESKY;
    } 
    else
    {
      printf("unknown solver type '%s'\n",sexp_item_symbol(value));
    }
  }
}


static void process_element_type(sexp_item* item, parse_data* data)
{
  sexp_item* value;
  value = sexp_item_attribute(item,"gauss-nodes-count");
  assert(value);
  data->fea_params->gauss_nodes_count = sexp_item_inumber(value);
  value = sexp_item_attribute(item,"nodes-count");
  assert(value);
  data->fea_params->nodes_per_element = sexp_item_inumber(value);
  value = sexp_item_attribute(item,"name");
  assert(value);
  if (sexp_item_is_symbol_like(value,"TETRAHEDRA10"))
    data->task->ele_type = TETRAHEDRA10;
}

static void process_line_search(sexp_item* item, parse_data* data)
{
  sexp_item* value;
  value = sexp_item_attribute(item,"max");
  assert(value);
  data->task->linesearch_max = sexp_item_inumber(value);
}

static void process_arc_length(sexp_item* item, parse_data* data)
{
  sexp_item* value;
  value = sexp_item_attribute(item,"max");
  assert(value);
  data->task->arclength_max = sexp_item_inumber(value);
}

static void process_nodes(sexp_item* item, parse_data* data)
{
  int count = 0;
  int i = 0;
  sexp_item* next = sexp_item_cdr(item);
  count = sexp_item_length(item) - 1;
  data->nodes->nodes_count = count;
  /* allocate storage for nodes */
  data->nodes->nodes =
    (real**)malloc(data->nodes->nodes_count*sizeof(real*));
  for (; i < data->nodes->nodes_count; ++ i)
  {
    data->nodes->nodes[i] = (real*)malloc(MAX_DOF*sizeof(real));
    item = sexp_item_car(next);
    assert(sexp_item_length(item) == 3);
    data->nodes->nodes[i][0] = sexp_item_fnumber(sexp_item_nth(item,0));
    data->nodes->nodes[i][1] = sexp_item_fnumber(sexp_item_nth(item,1));
    data->nodes->nodes[i][2] = sexp_item_fnumber(sexp_item_nth(item,2));
    next = sexp_item_cdr(next);
  }
}

static void process_elements(sexp_item* item, parse_data* data)
{
  int count = 0;
  int i = 0,j = 0;
  sexp_item* next = sexp_item_cdr(item);
  count = sexp_item_length(item) - 1;
  data->elements->elements_count = count;
  /* allocate storage for elements */
  data->elements->elements = 
    (int**)malloc(data->elements->elements_count*sizeof(int*));
  for (; i < data->elements->elements_count; ++ i)
  {
    data->elements->elements[i] =
      (int*)malloc(data->fea_params->nodes_per_element*sizeof(int));
    item = sexp_item_car(next);
    assert(sexp_item_length(item) == data->fea_params->nodes_per_element);
    for ( j = 0; j < data->fea_params->nodes_per_element; ++ j)
      data->elements->elements[i][j] =
        sexp_item_inumber(sexp_item_nth(item,j));
    next = sexp_item_cdr(next);
  }
}

static void process_prescribed(sexp_item* item, parse_data* data)
{
  int count = 0;
  int i = 0;
  int size;
  sexp_item* next = sexp_item_cdr(item);
  count = sexp_item_length(item) - 1;
  data->presc_boundary->prescribed_nodes_count = count;
  size = data->presc_boundary->prescribed_nodes_count;
  size = size*sizeof(prescribed_bnd_node);
  /* allocate storage for prescribed nodes */
  data->presc_boundary->prescribed_nodes =
    (prescribed_bnd_node*)malloc(size);
  for ( ; i < data->presc_boundary->prescribed_nodes_count; ++ i)
  {
    item = sexp_item_car(next);

    assert(sexp_item_starts_with_symbol(item,"presc-node"));
    data->presc_boundary->prescribed_nodes[i].node_number =
      sexp_item_inumber(sexp_item_attribute(item,"node-id"));
    data->presc_boundary->prescribed_nodes[i].values[0] =
      sexp_item_fnumber(sexp_item_attribute(item,"x"));
    data->presc_boundary->prescribed_nodes[i].values[1] =
      sexp_item_fnumber(sexp_item_attribute(item,"y"));
    data->presc_boundary->prescribed_nodes[i].values[2] =
      sexp_item_fnumber(sexp_item_attribute(item,"z"));
    data->presc_boundary->prescribed_nodes[i].type =
      (presc_boundary_type)
      sexp_item_inumber(sexp_item_attribute(item,"type"));

    next = sexp_item_cdr(next);
  }
}


static void traverse_function(sexp_item* item, void* data)
{
  parse_data* parse = (parse_data*)data;
  if (sexp_item_starts_with_symbol(item,"model"))
    process_model(item,parse);
  else if (sexp_item_starts_with_symbol(item,"model-parameters"))
    process_model_parameters(item,parse);
  else if (sexp_item_starts_with_symbol(item,"solution"))
    process_solution(item,parse);
  else if (sexp_item_starts_with_symbol(item,"slae-solver"))
    process_slae_solver(item,parse);
  else if (sexp_item_starts_with_symbol(item,"element-type"))
    process_element_type(item,parse);
  else if (sexp_item_starts_with_symbol(item,"line-search"))
    process_line_search(item,parse);
  else if (sexp_item_starts_with_symbol(item,"arc-length"))
    process_arc_length(item,parse);
  else if (sexp_item_starts_with_symbol(item,"nodes"))
    process_nodes(item,parse);
  else if (sexp_item_starts_with_symbol(item,"elements"))
    process_elements(item,parse);
  else if (sexp_item_starts_with_symbol(item,"prescribed-displacements"))
    process_prescribed(item,parse);
}


BOOL sexp_data_load(char *filename,
                    fea_task **task,
                    fea_solution_params **fea_params,
                    nodes_array **nodes,
                    elements_array **elements,
                    presc_bnd_array **presc_boundary)
{
  BOOL result = FALSE;
  FILE* sexp_document_file;
  sexp_item* sexp;
  parse_data parse;

  
  /* Try to open file */
  if (!(sexp_document_file = fopen(filename,"rt")))
  {
    fprintf(stderr,"Error, could not open file %s\n",filename);
    return FALSE;
  }
  /* parse input */
  sexp = sexp_parse_file(sexp_document_file);
  if (!sexp)
  {
    printf("Error: unable to parse SEXP input\n");
    return FALSE;
  }

  /* allocate parse data */
  parse.task = fea_task_alloc();
  parse.fea_params = fea_solution_params_alloc();
  parse.nodes = nodes_array_alloc();
  parse.elements = elements_array_alloc();
  parse.presc_boundary = presc_bnd_array_alloc();
  parse.current_size = 0;
  parse.current_text = (char*)0;

  if (sexp_item_starts_with_symbol(sexp,"task"))
  {
    sexp_item_traverse(sexp,traverse_function,&parse);
    *task = parse.task;
    *fea_params = parse.fea_params;
    *nodes = parse.nodes;
    *elements = parse.elements;
    *presc_boundary = parse.presc_boundary;

    
    result = TRUE;
  }

  sexp_item_free(sexp);

  return result;
}
