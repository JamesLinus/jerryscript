/* Copyright 2014 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "deserializer.h"
#include "ecma-gc.h"
#include "ecma-globals.h"
#include "ecma-global-object.h"
#include "ecma-helpers.h"
#include "ecma-lex-env.h"
#include "ecma-operations.h"
#include "globals.h"
#include "interpreter.h"
#include "jerry-libc.h"

#define __INIT_OP_FUNC(name, arg1, arg2, arg3) [ __op__idx_##name ] = opfunc_##name,
static const opfunc __opfuncs[LAST_OP] =
{
  OP_LIST (INIT_OP_FUNC)
};
#undef __INIT_OP_FUNC

JERRY_STATIC_ASSERT (sizeof (opcode_t) <= 4);

const opcode_t *__program = NULL;

/**
 * Initialize interpreter.
 */
void
init_int (const opcode_t *program_p) /**< pointer to byte-code program */
{
  JERRY_ASSERT (__program == NULL);

  __program = program_p;
} /* init_int */

bool
run_int (void)
{
  JERRY_ASSERT (__program != NULL);

  bool is_strict = false;
  opcode_counter_t start_pos = 0;

  opcode_t first_opcode = read_opcode (start_pos);
  if (first_opcode.op_idx == __op__idx_meta
      && first_opcode.data.meta.type == OPCODE_META_TYPE_STRICT_CODE)
  {
    is_strict = true;
    start_pos++;
  }

  ecma_object_t *glob_obj_p = ecma_op_create_global_object ();
  ecma_object_t *lex_env_p = ecma_op_create_global_environment (glob_obj_p);
  ecma_value_t this_binding_value = ecma_make_object_value (glob_obj_p);

  ecma_completion_value_t completion = run_int_from_pos (start_pos,
                                                         this_binding_value,
                                                         lex_env_p,
                                                         is_strict,
                                                         false);

  switch ((ecma_completion_type_t) completion.type)
  {
    case ECMA_COMPLETION_TYPE_NORMAL:
    case ECMA_COMPLETION_TYPE_META:
    {
      JERRY_UNREACHABLE ();
    }
    case ECMA_COMPLETION_TYPE_EXIT:
    {
      ecma_deref_object (glob_obj_p);
      ecma_deref_object (lex_env_p);
      ecma_finalize ();
      ecma_gc_run (ECMA_GC_GEN_COUNT - 1);

      return ecma_is_value_true (completion.value);
    }
    case ECMA_COMPLETION_TYPE_BREAK:
    case ECMA_COMPLETION_TYPE_CONTINUE:
    case ECMA_COMPLETION_TYPE_RETURN:
    {
      /* SyntaxError should be treated as an early error */
      JERRY_UNREACHABLE ();
    }
    case ECMA_COMPLETION_TYPE_THROW:
    {
      jerry_exit (ERR_UNHANDLED_EXCEPTION);
    }
  }

  JERRY_UNREACHABLE ();
}

ecma_completion_value_t
run_int_loop (int_data_t *int_data)
{
  while (true)
  {
    ecma_completion_value_t completion;

    do
    {
      const opcode_t *curr = &__program[int_data->pos];
      completion = __opfuncs[curr->op_idx] (*curr, int_data);

      JERRY_ASSERT (!ecma_is_completion_value_normal (completion)
                    || ecma_is_empty_completion_value (completion));
    }
    while (completion.type == ECMA_COMPLETION_TYPE_NORMAL);

    if (completion.type == ECMA_COMPLETION_TYPE_BREAK)
    {
      JERRY_UNIMPLEMENTED ();

      continue;
    }
    else if (completion.type == ECMA_COMPLETION_TYPE_CONTINUE)
    {
      JERRY_UNIMPLEMENTED ();

      continue;
    }

    if (completion.type == ECMA_COMPLETION_TYPE_META)
    {
      completion.type = ECMA_COMPLETION_TYPE_NORMAL;
      JERRY_ASSERT (ecma_is_completion_value_normal (completion));
    }

    return completion;
  }
}

ecma_completion_value_t
run_int_from_pos (opcode_counter_t start_pos,
                  ecma_value_t this_binding_value,
                  ecma_object_t *lex_env_p,
                  bool is_strict,
                  bool is_eval_code)
{
  ecma_completion_value_t completion;

  const opcode_t *curr = &__program[start_pos];
  JERRY_ASSERT (curr->op_idx == __op__idx_reg_var_decl);

  const idx_t min_reg_num = curr->data.reg_var_decl.min;
  const idx_t max_reg_num = curr->data.reg_var_decl.max;
  JERRY_ASSERT (max_reg_num >= min_reg_num);

  const uint32_t regs_num = (uint32_t) (max_reg_num - min_reg_num + 1);

  ecma_value_t regs[ regs_num ];

  /* memseting with zero initializes each 'register' to empty value */
  __memset (regs, 0, sizeof (regs));
  JERRY_ASSERT (ecma_is_value_empty (regs[0]));

  int_data_t int_data;
  int_data.pos = (opcode_counter_t) (start_pos + 1);
  int_data.this_binding = this_binding_value;
  int_data.lex_env_p = lex_env_p;
  int_data.is_strict = is_strict;
  int_data.is_eval_code = is_eval_code;
  int_data.min_reg_num = min_reg_num;
  int_data.max_reg_num = max_reg_num;
  int_data.regs_p = regs;

  completion = run_int_loop (&int_data);

  for (uint32_t reg_index = 0;
       reg_index < regs_num;
       reg_index++)
  {
    ecma_free_value (regs[ reg_index ], true);
  }

  return completion;
}

/**
 * Copy zero-terminated string literal value to specified buffer.
 *
 * @return upon success - number of bytes actually copied,
 *         otherwise (buffer size is not enough) - negated minimum required buffer size.
 */
ssize_t
try_get_string_by_idx (idx_t idx, /**< literal id */
                       ecma_char_t *buffer_p, /**< buffer */
                       ssize_t buffer_size) /**< buffer size */
{
  const ecma_char_t *str_p = deserialize_string_by_id (idx);
  JERRY_ASSERT (str_p != NULL);

  FIXME (ecma_char_t strlen);

  ssize_t req_length = (ssize_t)__strlen ((const char*)str_p) + 1;

  if (buffer_size < req_length)
  {
    return -req_length;
  }

  FIXME (ecma_char_t strncpy);

  JERRY_ASSERT (buffer_p != NULL);

  __strncpy ((char*)buffer_p, (const char*)str_p, (size_t)req_length);

  return req_length;
} /* try_get_string_by_idx */

/**
 * Get number literal value.
 *
 * @return value of number literal, corresponding to specified literal id
 */
ecma_number_t
get_number_by_idx (idx_t idx) /**< literal id */
{
  ecma_number_t num = deserialize_num_by_id (idx);

  return num;
} /* get_number_by_idx */

/**
 * Get specified opcode from the program.
 */
opcode_t
read_opcode (opcode_counter_t counter) /**< opcode counter */
{
  return __program[ counter ];
} /* read_opcode */
