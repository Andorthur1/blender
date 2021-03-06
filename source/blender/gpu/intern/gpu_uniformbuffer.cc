/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup gpu
 */

#include "MEM_guardedalloc.h"
#include <string.h>

#include "BLI_blenlib.h"
#include "BLI_math_base.h"

#include "gpu_context_private.hh"
#include "gpu_node_graph.h"

#include "GPU_extensions.h"
#include "GPU_glew.h"
#include "GPU_material.h"
#include "GPU_uniformbuffer.h"

typedef struct GPUUniformBuffer {
  /** Data size in bytes. */
  int size;
  /** GL handle for UBO. */
  GLuint bindcode;
  /** Current binding point. */
  int bindpoint;
  /** Continuous memory block to copy to GPU. Is own by the GPUUniformBuffer. */
  void *data;
} GPUUniformBuffer;

GPUUniformBuffer *GPU_uniformbuffer_create(int size, const void *data, char err_out[256])
{
  /* Make sure that UBO is padded to size of vec4 */
  BLI_assert((size % 16) == 0);

  if (size > GPU_max_ubo_size()) {
    if (err_out) {
      BLI_strncpy(err_out, "GPUUniformBuffer: UBO too big", 256);
    }
    return NULL;
  }

  GPUUniformBuffer *ubo = (GPUUniformBuffer *)MEM_mallocN(sizeof(GPUUniformBuffer), __func__);
  ubo->size = size;
  ubo->data = NULL;
  ubo->bindcode = 0;
  ubo->bindpoint = -1;

  /* Direct init. */
  if (data != NULL) {
    GPU_uniformbuffer_update(ubo, data);
  }

  return ubo;
}

void GPU_uniformbuffer_free(GPUUniformBuffer *ubo)
{
  MEM_SAFE_FREE(ubo->data);
  GPU_buf_free(ubo->bindcode);
  MEM_freeN(ubo);
}

/**
 * We need to pad some data types (vec3) on the C side
 * To match the GPU expected memory block alignment.
 */
static eGPUType get_padded_gpu_type(LinkData *link)
{
  GPUInput *input = (GPUInput *)link->data;
  eGPUType gputype = input->type;
  /* Unless the vec3 is followed by a float we need to treat it as a vec4. */
  if (gputype == GPU_VEC3 && (link->next != NULL) &&
      (((GPUInput *)link->next->data)->type != GPU_FLOAT)) {
    gputype = GPU_VEC4;
  }
  return gputype;
}

/**
 * Returns 1 if the first item should be after second item.
 * We make sure the vec4 uniforms come first.
 */
static int inputs_cmp(const void *a, const void *b)
{
  const LinkData *link_a = (const LinkData *)a, *link_b = (const LinkData *)b;
  const GPUInput *input_a = (const GPUInput *)link_a->data;
  const GPUInput *input_b = (const GPUInput *)link_b->data;
  return input_a->type < input_b->type ? 1 : 0;
}

/**
 * Make sure we respect the expected alignment of UBOs.
 * mat4, vec4, pad vec3 as vec4, then vec2, then floats.
 */
static void gpu_uniformbuffer_inputs_sort(ListBase *inputs)
{
/* Only support up to this type, if you want to extend it, make sure the
 * padding logic is correct for the new types. */
#define MAX_UBO_GPU_TYPE GPU_MAT4

  /* Order them as mat4, vec4, vec3, vec2, float. */
  BLI_listbase_sort(inputs, inputs_cmp);

  /* Creates a lookup table for the different types; */
  LinkData *inputs_lookup[MAX_UBO_GPU_TYPE + 1] = {NULL};
  eGPUType cur_type = static_cast<eGPUType>(MAX_UBO_GPU_TYPE + 1);

  LISTBASE_FOREACH (LinkData *, link, inputs) {
    GPUInput *input = (GPUInput *)link->data;

    if (input->type == GPU_MAT3) {
      /* Alignment for mat3 is not handled currently, so not supported */
      BLI_assert(!"mat3 not supported in UBO");
      continue;
    }
    if (input->type > MAX_UBO_GPU_TYPE) {
      BLI_assert(!"GPU type not supported in UBO");
      continue;
    }

    if (input->type == cur_type) {
      continue;
    }

    inputs_lookup[input->type] = link;
    cur_type = input->type;
  }

  /* If there is no GPU_VEC3 there is no need for alignment. */
  if (inputs_lookup[GPU_VEC3] == NULL) {
    return;
  }

  LinkData *link = inputs_lookup[GPU_VEC3];
  while (link != NULL && ((GPUInput *)link->data)->type == GPU_VEC3) {
    LinkData *link_next = link->next;

    /* If GPU_VEC3 is followed by nothing or a GPU_FLOAT, no need for alignment. */
    if ((link_next == NULL) || ((GPUInput *)link_next->data)->type == GPU_FLOAT) {
      break;
    }

    /* If there is a float, move it next to current vec3. */
    if (inputs_lookup[GPU_FLOAT] != NULL) {
      LinkData *float_input = inputs_lookup[GPU_FLOAT];
      inputs_lookup[GPU_FLOAT] = float_input->next;

      BLI_remlink(inputs, float_input);
      BLI_insertlinkafter(inputs, link, float_input);
    }

    link = link_next;
  }
#undef MAX_UBO_GPU_TYPE
}

/**
 * Create dynamic UBO from parameters
 * Return NULL if failed to create or if \param inputs: is empty.
 *
 * \param inputs: ListBase of #BLI_genericNodeN(#GPUInput).
 */
GPUUniformBuffer *GPU_uniformbuffer_dynamic_create(ListBase *inputs, char err_out[256])
{
  /* There is no point on creating an UBO if there is no arguments. */
  if (BLI_listbase_is_empty(inputs)) {
    return NULL;
  }
  /* Make sure we comply to the ubo alignment requirements. */
  gpu_uniformbuffer_inputs_sort(inputs);

  size_t buffer_size = 0;

  LISTBASE_FOREACH (LinkData *, link, inputs) {
    const eGPUType gputype = get_padded_gpu_type(link);
    buffer_size += gputype * sizeof(float);
  }
  /* Round up to size of vec4. (Opengl Requirement) */
  size_t alignment = sizeof(float[4]);
  buffer_size = divide_ceil_u(buffer_size, alignment) * alignment;
  void *data = MEM_mallocN(buffer_size, __func__);

  /* Now that we know the total ubo size we can start populating it. */
  float *offset = (float *)data;
  LISTBASE_FOREACH (LinkData *, link, inputs) {
    GPUInput *input = (GPUInput *)link->data;
    memcpy(offset, input->vec, input->type * sizeof(float));
    offset += get_padded_gpu_type(link);
  }

  /* Pass data as NULL for late init. */
  GPUUniformBuffer *ubo = GPU_uniformbuffer_create(buffer_size, NULL, err_out);
  /* Data will be update just before binding. */
  ubo->data = data;
  return ubo;
}

static void gpu_uniformbuffer_init(GPUUniformBuffer *ubo)
{
  BLI_assert(ubo->bindcode == 0);
  ubo->bindcode = GPU_buf_alloc();

  if (ubo->bindcode == 0) {
    fprintf(stderr, "GPUUniformBuffer: UBO create failed");
    BLI_assert(0);
    return;
  }

  glBindBuffer(GL_UNIFORM_BUFFER, ubo->bindcode);
  glBufferData(GL_UNIFORM_BUFFER, ubo->size, NULL, GL_DYNAMIC_DRAW);
}

void GPU_uniformbuffer_update(GPUUniformBuffer *ubo, const void *data)
{
  if (ubo->bindcode == 0) {
    gpu_uniformbuffer_init(ubo);
  }

  glBindBuffer(GL_UNIFORM_BUFFER, ubo->bindcode);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, ubo->size, data);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GPU_uniformbuffer_bind(GPUUniformBuffer *ubo, int number)
{
  if (number >= GPU_max_ubo_binds()) {
    fprintf(stderr, "Not enough UBO slots.\n");
    return;
  }

  if (ubo->bindcode == 0) {
    gpu_uniformbuffer_init(ubo);
  }

  if (ubo->data != NULL) {
    GPU_uniformbuffer_update(ubo, ubo->data);
    MEM_SAFE_FREE(ubo->data);
  }

  glBindBufferBase(GL_UNIFORM_BUFFER, number, ubo->bindcode);
  ubo->bindpoint = number;
}

void GPU_uniformbuffer_unbind(GPUUniformBuffer *ubo)
{
#ifndef NDEBUG
  glBindBufferBase(GL_UNIFORM_BUFFER, ubo->bindpoint, 0);
#endif
  ubo->bindpoint = 0;
}

void GPU_uniformbuffer_unbind_all(void)
{
  for (int i = 0; i < GPU_max_ubo_binds(); i++) {
    glBindBufferBase(GL_UNIFORM_BUFFER, i, 0);
  }
}
