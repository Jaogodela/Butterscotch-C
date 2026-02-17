#include "bs/vm/vm.h"

#include "bs/runtime/game_runner.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BS_VM_MAX_CALL_DEPTH 32u

typedef struct bs_vm_stack {
  bs_vm_value *items;
  size_t count;
  size_t capacity;
} bs_vm_stack;

typedef struct bs_vm_local_slot {
  int32_t variable_index;
  bs_vm_value value;
} bs_vm_local_slot;

typedef struct bs_vm_locals {
  bs_vm_local_slot *slots;
  size_t count;
  size_t capacity;
  const bs_vm_value *script_args;
  size_t script_argc;
  int32_t *array_variable_indices;
  int32_t *array_element_indices;
  bs_vm_value *array_values;
  size_t array_count;
  size_t array_capacity;
} bs_vm_locals;

typedef struct bs_vm_env_iteration {
  int32_t *instance_ids;
  size_t instance_count;
  size_t current_index;
  int32_t prev_self_id;
  int32_t prev_other_id;
} bs_vm_env_iteration;

static double bs_vm_now_millis(void) {
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
  }
  return 0.0;
}

typedef struct bs_vm_env_stack {
  bs_vm_env_iteration *items;
  size_t count;
  size_t capacity;
} bs_vm_env_stack;

static int32_t bs_decoded_lookup_instruction_index(const bs_decoded_code *decoded, uint32_t local_offset);
static bs_vm_value bs_vm_global_get_or_zero(const bs_vm *vm, int32_t variable_index);
static const char *bs_vm_variable_name(const bs_vm *vm, int32_t variable_index);
static bool bs_vm_builtin_array_get(const bs_vm *vm,
                                    int32_t variable_index,
                                    int32_t element_index,
                                    bs_vm_value *out_value);
static bool bs_vm_builtin_array_set(bs_vm *vm,
                                    int32_t variable_index,
                                    int32_t element_index,
                                    bs_vm_value value);
static bool bs_vm_instance_has_scalar(const bs_vm *vm, int32_t instance_id, int32_t variable_index);

typedef enum bs_vm_array_scope {
  BS_VM_ARRAY_SCOPE_INVALID = 0,
  BS_VM_ARRAY_SCOPE_LOCAL = 1,
  BS_VM_ARRAY_SCOPE_GLOBAL = 2,
  BS_VM_ARRAY_SCOPE_INSTANCE = 3
} bs_vm_array_scope;

static char *bs_vm_dup_cstr(const char *value) {
  size_t len = 0;
  char *copy = NULL;
  if (value == NULL) {
    return NULL;
  }

  len = strlen(value);
  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len + 1u);
  return copy;
}

static bs_vm_value bs_vm_value_zero(void) {
  bs_vm_value value = {0};
  value.type = BS_VM_VALUE_NUMBER;
  value.number = 0.0;
  value.string = NULL;
  return value;
}

static bs_vm_value bs_vm_value_number(double number) {
  bs_vm_value value = {0};
  value.type = BS_VM_VALUE_NUMBER;
  value.number = number;
  value.string = NULL;
  return value;
}

static bs_vm_value bs_vm_value_string(const char *string) {
  bs_vm_value value = {0};
  value.type = BS_VM_VALUE_STRING;
  value.number = 0.0;
  value.string = string;
  return value;
}

static double bs_vm_value_to_number(bs_vm_value value) {
  if (value.type == BS_VM_VALUE_STRING) {
    if (value.string == NULL || value.string[0] == '\0') {
      return 0.0;
    }
    return strtod(value.string, NULL);
  }
  return value.number;
}

static int64_t bs_vm_value_to_int64(bs_vm_value value) {
  return (int64_t)bs_vm_value_to_number(value);
}

static bool bs_vm_value_to_bool(bs_vm_value value) {
  if (value.type == BS_VM_VALUE_STRING) {
    return value.string != NULL && value.string[0] != '\0';
  }
  return value.number != 0.0;
}

static bool bs_vm_store_owned_string(bs_vm *vm, const char *value, const char **out_owned) {
  char **grown = NULL;
  char *copy = NULL;
  if (out_owned == NULL) {
    return false;
  }

  *out_owned = "";
  if (vm == NULL) {
    return false;
  }
  if (value == NULL) {
    value = "";
  }

  for (size_t i = 0; i < vm->owned_string_count; i++) {
    if (vm->owned_strings[i] != NULL && strcmp(vm->owned_strings[i], value) == 0) {
      *out_owned = vm->owned_strings[i];
      return true;
    }
  }

  if (vm->owned_string_count == vm->owned_string_capacity) {
    size_t new_capacity = (vm->owned_string_capacity == 0) ? 128u : (vm->owned_string_capacity * 2u);
    grown = (char **)realloc(vm->owned_strings, new_capacity * sizeof(char *));
    if (grown == NULL) {
      return false;
    }
    vm->owned_strings = grown;
    vm->owned_string_capacity = new_capacity;
  }

  copy = bs_vm_dup_cstr(value);
  if (copy == NULL) {
    return false;
  }

  vm->owned_strings[vm->owned_string_count] = copy;
  vm->owned_string_count++;
  *out_owned = copy;
  return true;
}

static bool bs_vm_make_storable_value(bs_vm *vm, bs_vm_value value, bs_vm_value *out_value) {
  const char *owned = NULL;
  if (out_value == NULL) {
    return false;
  }
  if (value.type != BS_VM_VALUE_STRING) {
    *out_value = bs_vm_value_number(value.number);
    return true;
  }
  if (!bs_vm_store_owned_string(vm, value.string, &owned)) {
    return false;
  }
  *out_value = bs_vm_value_string(owned);
  return true;
}

static bool bs_vm_trace_writer_enabled(void) {
  static int initialized = 0;
  static bool enabled = false;
  if (!initialized) {
    const char *env = getenv("BS_TRACE_WRITER");
    enabled = (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
    initialized = 1;
  }
  return enabled;
}

static void bs_vm_trace_writer_set(const bs_vm *vm,
                                   int32_t instance_id,
                                   int32_t variable_index,
                                   int32_t element_index,
                                   bool is_array,
                                   bs_vm_value value) {
  const char *name = NULL;
  if (vm == NULL || !bs_vm_trace_writer_enabled()) {
    return;
  }
  name = bs_vm_variable_name(vm, variable_index);
  if (name == NULL) {
    return;
  }
  if (strcmp(name, "mystring") != 0 &&
      strcmp(name, "originalstring") != 0 &&
      strcmp(name, "stringpos") != 0 &&
      strcmp(name, "textstring") != 0) {
    return;
  }

  if (value.type == BS_VM_VALUE_STRING) {
    if (is_array) {
      printf("  [WRITER SET] inst=%d %s[%d]=\"%s\"\n",
             instance_id,
             name,
             element_index,
             value.string != NULL ? value.string : "");
    } else {
      printf("  [WRITER SET] inst=%d %s=\"%s\"\n",
             instance_id,
             name,
             value.string != NULL ? value.string : "");
    }
  } else {
    if (is_array) {
      printf("  [WRITER SET] inst=%d %s[%d]=%.3f\n",
             instance_id,
             name,
             element_index,
             value.number);
    } else {
      printf("  [WRITER SET] inst=%d %s=%.3f\n",
             instance_id,
             name,
             value.number);
    }
  }
}

static void bs_vm_stack_dispose(bs_vm_stack *stack) {
  if (stack == NULL) {
    return;
  }
  free(stack->items);
  stack->items = NULL;
  stack->count = 0;
  stack->capacity = 0;
}

static bool bs_vm_stack_push(bs_vm_stack *stack, bs_vm_value value) {
  if (stack == NULL) {
    return false;
  }

  if (stack->count == stack->capacity) {
    size_t new_capacity = (stack->capacity == 0) ? 128u : (stack->capacity * 2u);
    bs_vm_value *grown = (bs_vm_value *)realloc(stack->items, new_capacity * sizeof(bs_vm_value));
    if (grown == NULL) {
      return false;
    }
    stack->items = grown;
    stack->capacity = new_capacity;
  }

  stack->items[stack->count] = value;
  stack->count++;
  return true;
}

static bs_vm_value bs_vm_stack_pop_or_zero(bs_vm_stack *stack) {
  if (stack == NULL || stack->count == 0) {
    return bs_vm_value_zero();
  }
  stack->count--;
  return stack->items[stack->count];
}

static bs_vm_value bs_vm_stack_peek_or_zero(const bs_vm_stack *stack) {
  if (stack == NULL || stack->count == 0) {
    return bs_vm_value_zero();
  }
  return stack->items[stack->count - 1];
}

static void bs_vm_locals_dispose(bs_vm_locals *locals) {
  if (locals == NULL) {
    return;
  }
  free(locals->slots);
  free(locals->array_variable_indices);
  free(locals->array_element_indices);
  free(locals->array_values);
  locals->slots = NULL;
  locals->count = 0;
  locals->capacity = 0;
  locals->script_args = NULL;
  locals->script_argc = 0;
  locals->array_variable_indices = NULL;
  locals->array_element_indices = NULL;
  locals->array_values = NULL;
  locals->array_count = 0;
  locals->array_capacity = 0;
}

static void bs_vm_env_stack_dispose(bs_vm_env_stack *stack) {
  if (stack == NULL) {
    return;
  }
  if (stack->items != NULL) {
    for (size_t i = 0; i < stack->count; i++) {
      free(stack->items[i].instance_ids);
      stack->items[i].instance_ids = NULL;
      stack->items[i].instance_count = 0;
      stack->items[i].current_index = 0;
    }
  }
  free(stack->items);
  stack->items = NULL;
  stack->count = 0;
  stack->capacity = 0;
}

static bool bs_vm_env_stack_push(bs_vm_env_stack *stack, bs_vm_env_iteration frame) {
  if (stack == NULL) {
    return false;
  }
  if (stack->count == stack->capacity) {
    size_t new_capacity = (stack->capacity == 0) ? 8u : (stack->capacity * 2u);
    bs_vm_env_iteration *grown =
        (bs_vm_env_iteration *)realloc(stack->items, new_capacity * sizeof(bs_vm_env_iteration));
    if (grown == NULL) {
      return false;
    }
    stack->items = grown;
    stack->capacity = new_capacity;
  }
  stack->items[stack->count] = frame;
  stack->count++;
  return true;
}

static bs_vm_env_iteration *bs_vm_env_stack_last(bs_vm_env_stack *stack) {
  if (stack == NULL || stack->count == 0) {
    return NULL;
  }
  return &stack->items[stack->count - 1u];
}

static void bs_vm_env_stack_pop(bs_vm_env_stack *stack) {
  if (stack == NULL || stack->count == 0) {
    return;
  }
  stack->count--;
  free(stack->items[stack->count].instance_ids);
  stack->items[stack->count].instance_ids = NULL;
  stack->items[stack->count].instance_count = 0;
  stack->items[stack->count].current_index = 0;
}

static bool bs_vm_locals_set(bs_vm *vm, bs_vm_locals *locals, int32_t variable_index, bs_vm_value value) {
  bs_vm_value stored_value = bs_vm_value_zero();
  if (vm == NULL || locals == NULL || variable_index < 0) {
    return false;
  }
  if (!bs_vm_make_storable_value(vm, value, &stored_value)) {
    return false;
  }

  for (size_t i = 0; i < locals->count; i++) {
    if (locals->slots[i].variable_index == variable_index) {
      locals->slots[i].value = stored_value;
      return true;
    }
  }

  if (locals->count == locals->capacity) {
    size_t new_capacity = (locals->capacity == 0) ? 64u : (locals->capacity * 2u);
    bs_vm_local_slot *grown = (bs_vm_local_slot *)realloc(locals->slots, new_capacity * sizeof(bs_vm_local_slot));
    if (grown == NULL) {
      return false;
    }
    locals->slots = grown;
    locals->capacity = new_capacity;
  }

  locals->slots[locals->count].variable_index = variable_index;
  locals->slots[locals->count].value = stored_value;
  locals->count++;
  return true;
}

static bs_vm_value bs_vm_locals_get_or_zero(const bs_vm_locals *locals, int32_t variable_index) {
  if (locals == NULL || variable_index < 0) {
    return bs_vm_value_zero();
  }

  for (size_t i = 0; i < locals->count; i++) {
    if (locals->slots[i].variable_index == variable_index) {
      return locals->slots[i].value;
    }
  }

  return bs_vm_value_zero();
}

static bs_vm_value bs_vm_locals_array_get_or_zero(const bs_vm_locals *locals,
                                                  int32_t variable_index,
                                                  int32_t element_index) {
  if (locals == NULL || variable_index < 0) {
    return bs_vm_value_zero();
  }

  for (size_t i = 0; i < locals->array_count; i++) {
    if (locals->array_variable_indices[i] == variable_index &&
        locals->array_element_indices[i] == element_index) {
      return locals->array_values[i];
    }
  }
  return bs_vm_value_zero();
}

static bool bs_vm_locals_array_set(bs_vm *vm,
                                   bs_vm_locals *locals,
                                   int32_t variable_index,
                                   int32_t element_index,
                                   bs_vm_value value) {
  bs_vm_value stored_value = bs_vm_value_zero();
  if (vm == NULL || locals == NULL || variable_index < 0) {
    return false;
  }
  if (!bs_vm_make_storable_value(vm, value, &stored_value)) {
    return false;
  }

  for (size_t i = 0; i < locals->array_count; i++) {
    if (locals->array_variable_indices[i] == variable_index &&
        locals->array_element_indices[i] == element_index) {
      locals->array_values[i] = stored_value;
      return true;
    }
  }

  if (locals->array_count == locals->array_capacity) {
    size_t new_capacity = (locals->array_capacity == 0) ? 64u : (locals->array_capacity * 2u);
    int32_t *grown_var_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    int32_t *grown_elem_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    bs_vm_value *grown_values = (bs_vm_value *)malloc(new_capacity * sizeof(bs_vm_value));
    if (grown_var_indices == NULL || grown_elem_indices == NULL || grown_values == NULL) {
      free(grown_var_indices);
      free(grown_elem_indices);
      free(grown_values);
      return false;
    }

    if (locals->array_count > 0) {
      memcpy(grown_var_indices,
             locals->array_variable_indices,
             locals->array_count * sizeof(int32_t));
      memcpy(grown_elem_indices,
             locals->array_element_indices,
             locals->array_count * sizeof(int32_t));
      memcpy(grown_values,
             locals->array_values,
             locals->array_count * sizeof(bs_vm_value));
    }

    free(locals->array_variable_indices);
    free(locals->array_element_indices);
    free(locals->array_values);
    locals->array_variable_indices = grown_var_indices;
    locals->array_element_indices = grown_elem_indices;
    locals->array_values = grown_values;
    locals->array_capacity = new_capacity;
  }

  locals->array_variable_indices[locals->array_count] = variable_index;
  locals->array_element_indices[locals->array_count] = element_index;
  locals->array_values[locals->array_count] = stored_value;
  locals->array_count++;
  return true;
}

static bool bs_vm_locals_has_scalar(const bs_vm_locals *locals, int32_t variable_index) {
  if (locals == NULL || variable_index < 0) {
    return false;
  }
  for (size_t i = 0; i < locals->count; i++) {
    if (locals->slots[i].variable_index == variable_index) {
      return true;
    }
  }
  return false;
}

static bool bs_vm_locals_has_array(const bs_vm_locals *locals, int32_t variable_index) {
  if (locals == NULL || variable_index < 0) {
    return false;
  }
  for (size_t i = 0; i < locals->array_count; i++) {
    if (locals->array_variable_indices[i] == variable_index) {
      return true;
    }
  }
  return false;
}

static void bs_vm_locals_array_clear_variable(bs_vm_locals *locals, int32_t variable_index) {
  size_t write = 0;
  if (locals == NULL || variable_index < 0) {
    return;
  }
  for (size_t i = 0; i < locals->array_count; i++) {
    if (locals->array_variable_indices[i] == variable_index) {
      continue;
    }
    if (write != i) {
      locals->array_variable_indices[write] = locals->array_variable_indices[i];
      locals->array_element_indices[write] = locals->array_element_indices[i];
      locals->array_values[write] = locals->array_values[i];
    }
    write++;
  }
  locals->array_count = write;
}

static bool bs_vm_make_array_ref_value(bs_vm *vm,
                                       bs_vm_array_scope scope,
                                       int32_t instance_id,
                                       int32_t variable_index,
                                       bs_vm_value *out_value) {
  char encoded[80];
  const char *owned = NULL;
  if (vm == NULL || out_value == NULL || variable_index < 0) {
    return false;
  }
  (void)snprintf(encoded,
                 sizeof(encoded),
                 "__bs_arrref:%d:%d:%d",
                 (int)scope,
                 (int)instance_id,
                 (int)variable_index);
  if (!bs_vm_store_owned_string(vm, encoded, &owned)) {
    return false;
  }
  *out_value = bs_vm_value_string(owned);
  return true;
}

static bool bs_vm_parse_array_ref_value(bs_vm_value value,
                                        bs_vm_array_scope *out_scope,
                                        int32_t *out_instance_id,
                                        int32_t *out_variable_index) {
  static const char prefix[] = "__bs_arrref:";
  const size_t prefix_len = sizeof(prefix) - 1u;
  int parsed_scope = 0;
  int parsed_instance = 0;
  int parsed_variable = 0;
  if (value.type != BS_VM_VALUE_STRING || value.string == NULL) {
    return false;
  }
  if (strncmp(value.string, prefix, prefix_len) != 0) {
    return false;
  }
  if (sscanf(value.string + prefix_len, "%d:%d:%d", &parsed_scope, &parsed_instance, &parsed_variable) != 3) {
    return false;
  }
  if (parsed_variable < 0 || parsed_scope < (int)BS_VM_ARRAY_SCOPE_LOCAL || parsed_scope > (int)BS_VM_ARRAY_SCOPE_INSTANCE) {
    return false;
  }
  if (out_scope != NULL) {
    *out_scope = (bs_vm_array_scope)parsed_scope;
  }
  if (out_instance_id != NULL) {
    *out_instance_id = (int32_t)parsed_instance;
  }
  if (out_variable_index != NULL) {
    *out_variable_index = (int32_t)parsed_variable;
  }
  return true;
}

static bs_vm_value bs_vm_locals_argument_get_or_zero(const bs_vm_locals *locals, int32_t arg_index) {
  if (locals == NULL ||
      arg_index < 0 ||
      locals->script_args == NULL ||
      (size_t)arg_index >= locals->script_argc) {
    return bs_vm_value_zero();
  }
  return locals->script_args[(size_t)arg_index];
}

static bool bs_vm_locals_seed_script_arguments(bs_vm *vm,
                                               bs_vm_locals *locals,
                                               const bs_vm_value *args,
                                               size_t argc) {
  if (vm == NULL || locals == NULL) {
    return false;
  }

  locals->script_args = args;
  locals->script_argc = argc;

  if (vm->argument_count_variable_index >= 0 &&
      !bs_vm_locals_set(vm,
                        locals,
                        vm->argument_count_variable_index,
                        bs_vm_value_number((double)argc))) {
    return false;
  }

  for (size_t i = 0; i < 16u; i++) {
    int32_t variable_index = vm->argument_slot_variable_indices[i];
    bs_vm_value value = bs_vm_value_zero();
    if (variable_index < 0) {
      continue;
    }
    if (i < argc && args != NULL) {
      value = args[i];
    }
    if (!bs_vm_locals_set(vm, locals, variable_index, value)) {
      return false;
    }
  }

  return true;
}

static bool bs_vm_variable_is_global(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL ||
      vm->game_data == NULL ||
      variable_index < 0 ||
      (size_t)variable_index >= vm->game_data->variable_count) {
    return false;
  }
  return vm->game_data->variables[(size_t)variable_index].instance_type == BS_INSTANCE_GLOBAL;
}

static const char *bs_vm_variable_name(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL ||
      vm->game_data == NULL ||
      variable_index < 0 ||
      (size_t)variable_index >= vm->game_data->variable_count) {
    return NULL;
  }
  return vm->game_data->variables[(size_t)variable_index].name;
}

static int32_t bs_vm_find_variable_index_by_name(const bs_vm *vm, const char *name) {
  if (vm == NULL || vm->game_data == NULL || name == NULL) {
    return -1;
  }
  for (size_t i = 0; i < vm->game_data->variable_count; i++) {
    const char *var_name = vm->game_data->variables[i].name;
    if (var_name != NULL && strcmp(var_name, name) == 0) {
      return (int32_t)i;
    }
  }
  return -1;
}

static void bs_vm_cache_argument_variable_indices(bs_vm *vm) {
  if (vm == NULL) {
    return;
  }

  vm->argument_array_variable_index = bs_vm_find_variable_index_by_name(vm, "argument");
  vm->argument_count_variable_index = bs_vm_find_variable_index_by_name(vm, "argument_count");
  for (int i = 0; i < 16; i++) {
    char name[16];
    (void)snprintf(name, sizeof(name), "argument%d", i);
    vm->argument_slot_variable_indices[i] = bs_vm_find_variable_index_by_name(vm, name);
  }
}

static bool bs_vm_variable_is_argument_array(const bs_vm *vm, int32_t variable_index) {
  return vm != NULL && variable_index >= 0 && vm->argument_array_variable_index == variable_index;
}

static bool bs_vm_variable_is_argument_slot(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL || variable_index < 0) {
    return false;
  }
  if (vm->argument_count_variable_index == variable_index) {
    return true;
  }
  for (size_t i = 0; i < 16u; i++) {
    if (vm->argument_slot_variable_indices[i] == variable_index) {
      return true;
    }
  }
  return false;
}

static int32_t bs_vm_variable_instance_type(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL ||
      vm->game_data == NULL ||
      variable_index < 0 ||
      (size_t)variable_index >= vm->game_data->variable_count) {
    return BS_INSTANCE_SELF;
  }
  return vm->game_data->variables[(size_t)variable_index].instance_type;
}

static int32_t bs_vm_variable_effective_instance_type(const bs_vm *vm, const bs_instruction *instr) {
  if (instr == NULL) {
    return BS_INSTANCE_SELF;
  }
  if (instr->extra != 0) {
    return (int32_t)instr->extra;
  }
  return bs_vm_variable_instance_type(vm, instr->variable_index);
}

static bool bs_vm_variable_is_alarm(const bs_vm *vm, int32_t variable_index) {
  const char *name = bs_vm_variable_name(vm, variable_index);
  if (name == NULL) {
    return false;
  }
  return name != NULL && strcmp(name, "alarm") == 0;
}

static bool bs_vm_instance_variable_is_runner_managed(const char *name) {
  if (name == NULL) {
    return false;
  }

  return strcmp(name, "x") == 0 ||
         strcmp(name, "y") == 0 ||
         strcmp(name, "xprevious") == 0 ||
         strcmp(name, "yprevious") == 0 ||
         strcmp(name, "xstart") == 0 ||
         strcmp(name, "ystart") == 0 ||
         strcmp(name, "hspeed") == 0 ||
         strcmp(name, "vspeed") == 0 ||
         strcmp(name, "speed") == 0 ||
         strcmp(name, "direction") == 0 ||
         strcmp(name, "friction") == 0 ||
         strcmp(name, "gravity") == 0 ||
         strcmp(name, "gravity_direction") == 0 ||
         strcmp(name, "id") == 0 ||
         strcmp(name, "object_index") == 0 ||
         strcmp(name, "sprite_index") == 0 ||
         strcmp(name, "mask_index") == 0 ||
         strcmp(name, "depth") == 0 ||
         strcmp(name, "visible") == 0 ||
         strcmp(name, "solid") == 0 ||
         strcmp(name, "persistent") == 0 ||
         strcmp(name, "image_index") == 0 ||
         strcmp(name, "image_speed") == 0 ||
         strcmp(name, "image_xscale") == 0 ||
         strcmp(name, "image_yscale") == 0 ||
         strcmp(name, "image_angle") == 0 ||
         strcmp(name, "image_alpha") == 0 ||
         strcmp(name, "image_single") == 0 ||
         strcmp(name, "image_blend") == 0 ||
         strcmp(name, "image_number") == 0 ||
         strcmp(name, "path_index") == 0 ||
         strcmp(name, "path_position") == 0 ||
         strcmp(name, "path_speed") == 0 ||
         strcmp(name, "path_endaction") == 0 ||
         strcmp(name, "path_orientation") == 0 ||
         strcmp(name, "path_scale") == 0 ||
         strcmp(name, "room_persistent") == 0 ||
         strcmp(name, "bbox_left") == 0 ||
         strcmp(name, "bbox_right") == 0 ||
         strcmp(name, "bbox_top") == 0 ||
         strcmp(name, "bbox_bottom") == 0 ||
         strcmp(name, "sprite_width") == 0 ||
         strcmp(name, "sprite_height") == 0;
}

static bool bs_vm_instruction_is_array(const bs_instruction *instr) {
  if (instr == NULL) {
    return false;
  }
  return instr->variable_type == 0x00;
}

static bool bs_vm_instruction_is_stacktop(const bs_instruction *instr) {
  if (instr == NULL) {
    return false;
  }
  return instr->variable_type == 0x80;
}

static int32_t bs_vm_resolve_single_instance_target(bs_vm *vm, int32_t instance_target) {
  if (vm == NULL || vm->runner == NULL) {
    return -4;
  }

  switch (instance_target) {
    case BS_INSTANCE_SELF:
    case BS_INSTANCE_BUILTIN:
      return vm->current_self_id;
    case BS_INSTANCE_OTHER:
      return vm->current_other_id;
    case BS_INSTANCE_ALL:
      return -4;
    case BS_INSTANCE_NOONE:
    case BS_INSTANCE_GLOBAL:
    case BS_INSTANCE_LOCAL:
      return -4;
    default:
      break;
  }

  if (instance_target >= 100000) {
    return instance_target;
  }
  if (instance_target >= 0) {
    for (size_t i = 0; i < vm->runner->instance_count; i++) {
      const bs_instance *inst = &vm->runner->instances[i];
      if (inst->destroyed) {
        continue;
      }
      if (bs_game_runner_object_is_child_of(vm->runner, inst->object_index, instance_target)) {
        return inst->id;
      }
    }
    return -4;
  }

  return vm->current_self_id;
}

static bool bs_vm_try_get_known_global_builtin(const bs_vm *vm,
                                               int32_t variable_index,
                                               double *out_value) {
  const char *name = bs_vm_variable_name(vm, variable_index);
  if (vm == NULL ||
      vm->runner == NULL ||
      vm->game_data == NULL ||
      name == NULL ||
      out_value == NULL) {
    return false;
  }

  if (strcmp(name, "room") == 0) {
    *out_value = (double)vm->runner->current_room_index;
    return true;
  }
  if (strcmp(name, "room_speed") == 0) {
    *out_value = (double)(vm->runner->current_room != NULL ? vm->runner->current_room->speed : 30);
    return true;
  }
  if (strcmp(name, "room_width") == 0) {
    *out_value = (double)(vm->runner->current_room != NULL ? vm->runner->current_room->width : 640);
    return true;
  }
  if (strcmp(name, "room_height") == 0) {
    *out_value = (double)(vm->runner->current_room != NULL ? vm->runner->current_room->height : 480);
    return true;
  }
  if (strcmp(name, "view_current") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "current_time") == 0) {
    *out_value = bs_vm_now_millis();
    return true;
  }
  if (strcmp(name, "fps") == 0) {
    *out_value = (double)(vm->runner->current_room != NULL ? vm->runner->current_room->speed : 30);
    return true;
  }
  if (strcmp(name, "instance_count") == 0) {
    *out_value = (double)vm->runner->instance_count;
    return true;
  }
  if (strcmp(name, "keyboard_key") == 0) {
    *out_value = (double)vm->runner->keyboard_key;
    return true;
  }
  if (strcmp(name, "keyboard_lastkey") == 0) {
    *out_value = (double)vm->runner->keyboard_lastkey;
    return true;
  }
  if (strcmp(name, "mouse_x") == 0 || strcmp(name, "mouse_y") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "os_type") == 0) {
    *out_value = 1.0;
    return true;
  }
  if (strcmp(name, "game_id") == 0) {
    *out_value = (double)vm->game_data->gen8.game_id;
    return true;
  }
  if (strcmp(name, "browser_width") == 0) {
    *out_value = (double)vm->game_data->gen8.window_width;
    return true;
  }
  if (strcmp(name, "browser_height") == 0) {
    *out_value = (double)vm->game_data->gen8.window_height;
    return true;
  }
  if (strcmp(name, "room_persistent") == 0) {
    if (vm->runner->current_room_index >= 0 &&
        (size_t)vm->runner->current_room_index < vm->runner->room_persistent_flag_count &&
        vm->runner->room_persistent_flags != NULL) {
      *out_value = vm->runner->room_persistent_flags[(size_t)vm->runner->current_room_index] ? 1.0 : 0.0;
    } else {
      *out_value = (vm->runner->current_room != NULL && vm->runner->current_room->persistent) ? 1.0 : 0.0;
    }
    return true;
  }
  if (strcmp(name, "display_aa") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "application_surface") == 0) {
    *out_value = -1.0;
    return true;
  }
  if (strcmp(name, "path_action_stop") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "path_action_restart") == 0) {
    *out_value = 1.0;
    return true;
  }
  if (strcmp(name, "path_action_continue") == 0) {
    *out_value = 2.0;
    return true;
  }
  if (strcmp(name, "path_action_reverse") == 0) {
    *out_value = 3.0;
    return true;
  }

  return false;
}

static const bs_room_view_data *bs_vm_builtin_view_get(const bs_vm *vm, int32_t index) {
  const bs_room_data *room = NULL;
  if (vm == NULL ||
      vm->runner == NULL ||
      vm->runner->current_room == NULL ||
      index < 0) {
    return NULL;
  }
  room = vm->runner->current_room;
  if (room->views == NULL || (size_t)index >= room->view_count) {
    return NULL;
  }
  return &room->views[(size_t)index];
}

static bool bs_vm_builtin_array_get(const bs_vm *vm,
                                    int32_t variable_index,
                                    int32_t element_index,
                                    bs_vm_value *out_value) {
  const char *name = bs_vm_variable_name(vm, variable_index);
  const bs_room_view_data *view = bs_vm_builtin_view_get(vm, element_index);
  if (out_value == NULL || name == NULL) {
    return false;
  }

  if (strcmp(name, "view_wview") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->view_w : 640));
    return true;
  }
  if (strcmp(name, "view_hview") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->view_h : 480));
    return true;
  }
  if (strcmp(name, "view_xview") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->view_x : 0));
    return true;
  }
  if (strcmp(name, "view_yview") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->view_y : 0));
    return true;
  }
  if (strcmp(name, "view_wport") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->port_w : 640));
    return true;
  }
  if (strcmp(name, "view_hport") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->port_h : 480));
    return true;
  }
  if (strcmp(name, "view_xport") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->port_x : 0));
    return true;
  }
  if (strcmp(name, "view_yport") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->port_y : 0));
    return true;
  }
  if (strcmp(name, "view_hborder") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->border_h : 0));
    return true;
  }
  if (strcmp(name, "view_vborder") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->border_v : 0));
    return true;
  }
  if (strcmp(name, "view_hspeed") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->speed_h : 0));
    return true;
  }
  if (strcmp(name, "view_vspeed") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->speed_v : 0));
    return true;
  }
  if (strcmp(name, "view_object") == 0) {
    *out_value = bs_vm_value_number((double)(view != NULL ? view->follow_object_id : -1));
    return true;
  }
  if (strcmp(name, "view_visible") == 0) {
    *out_value = bs_vm_value_number((view != NULL && view->enabled) ? 1.0 : 0.0);
    return true;
  }
  return false;
}

static bool bs_vm_builtin_array_set(bs_vm *vm,
                                    int32_t variable_index,
                                    int32_t element_index,
                                    bs_vm_value value) {
  const char *name = bs_vm_variable_name(vm, variable_index);
  bs_room_view_data *view = NULL;
  if (vm == NULL ||
      vm->runner == NULL ||
      vm->runner->current_room == NULL ||
      vm->runner->current_room->views == NULL ||
      element_index < 0 ||
      (size_t)element_index >= vm->runner->current_room->view_count ||
      name == NULL) {
    return false;
  }
  view = &vm->runner->current_room->views[(size_t)element_index];

  if (strcmp(name, "view_xview") == 0) {
    view->view_x = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_yview") == 0) {
    view->view_y = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_wview") == 0) {
    view->view_w = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_hview") == 0) {
    view->view_h = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_wport") == 0) {
    view->port_w = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_hport") == 0) {
    view->port_h = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_xport") == 0) {
    view->port_x = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_yport") == 0) {
    view->port_y = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_hborder") == 0) {
    view->border_h = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_vborder") == 0) {
    view->border_v = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_hspeed") == 0) {
    view->speed_h = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_vspeed") == 0) {
    view->speed_v = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_object") == 0) {
    view->follow_object_id = (int32_t)bs_vm_value_to_number(value);
    return true;
  }
  if (strcmp(name, "view_visible") == 0) {
    view->enabled = bs_vm_value_to_number(value) > 0.5;
    return true;
  }
  return false;
}

static bs_vm_value bs_vm_global_or_builtin_get_or_zero(const bs_vm *vm, int32_t variable_index) {
  double builtin_value = 0.0;
  if (bs_vm_try_get_known_global_builtin(vm, variable_index, &builtin_value)) {
    return bs_vm_value_number(builtin_value);
  }
  return bs_vm_global_get_or_zero(vm, variable_index);
}

static bs_vm_value bs_vm_instance_dynamic_get_or_zero(bs_vm *vm,
                                                      int32_t variable_index,
                                                      int32_t target_instance_id) {
  bs_instance *instance = NULL;
  if (vm == NULL ||
      vm->runner == NULL ||
      target_instance_id < 0 ||
      variable_index < 0) {
    return bs_vm_value_zero();
  }

  instance = bs_game_runner_find_instance_by_id(vm->runner, target_instance_id);
  if (instance == NULL || instance->destroyed) {
    return bs_vm_value_zero();
  }

  for (size_t i = 0; i < vm->instance_variable_count; i++) {
    if (vm->instance_variable_instance_ids[i] == target_instance_id &&
        vm->instance_variable_indices[i] == variable_index) {
      return vm->instance_variable_values[i];
    }
  }
  return bs_vm_value_zero();
}

static bool bs_vm_instance_dynamic_set(bs_vm *vm,
                                       int32_t variable_index,
                                       int32_t target_instance_id,
                                       bs_vm_value value) {
  bs_instance *instance = NULL;
  bs_vm_value stored_value = bs_vm_value_zero();
  if (vm == NULL ||
      vm->runner == NULL ||
      target_instance_id < 0 ||
      variable_index < 0) {
    return false;
  }

  instance = bs_game_runner_find_instance_by_id(vm->runner, target_instance_id);
  if (instance == NULL || instance->destroyed) {
    return false;
  }

  if (!bs_vm_make_storable_value(vm, value, &stored_value)) {
    return false;
  }

  for (size_t i = 0; i < vm->instance_variable_count; i++) {
    if (vm->instance_variable_instance_ids[i] == target_instance_id &&
        vm->instance_variable_indices[i] == variable_index) {
      vm->instance_variable_values[i] = stored_value;
      bs_vm_trace_writer_set(vm,
                             target_instance_id,
                             variable_index,
                             -1,
                             false,
                             stored_value);
      return true;
    }
  }

  if (vm->instance_variable_count == vm->instance_variable_capacity) {
    size_t new_capacity = (vm->instance_variable_capacity == 0) ? 128u : (vm->instance_variable_capacity * 2u);
    int32_t *grown_instance_ids = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    int32_t *grown_variable_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    bs_vm_value *grown_values = (bs_vm_value *)malloc(new_capacity * sizeof(bs_vm_value));
    if (grown_instance_ids == NULL || grown_variable_indices == NULL || grown_values == NULL) {
      free(grown_instance_ids);
      free(grown_variable_indices);
      free(grown_values);
      return false;
    }

    if (vm->instance_variable_count > 0) {
      memcpy(grown_instance_ids,
             vm->instance_variable_instance_ids,
             vm->instance_variable_count * sizeof(int32_t));
      memcpy(grown_variable_indices,
             vm->instance_variable_indices,
             vm->instance_variable_count * sizeof(int32_t));
      memcpy(grown_values,
             vm->instance_variable_values,
             vm->instance_variable_count * sizeof(bs_vm_value));
    }

    free(vm->instance_variable_instance_ids);
    free(vm->instance_variable_indices);
    free(vm->instance_variable_values);
    vm->instance_variable_instance_ids = grown_instance_ids;
    vm->instance_variable_indices = grown_variable_indices;
    vm->instance_variable_values = grown_values;
    vm->instance_variable_capacity = new_capacity;
  }

  vm->instance_variable_instance_ids[vm->instance_variable_count] = target_instance_id;
  vm->instance_variable_indices[vm->instance_variable_count] = variable_index;
  vm->instance_variable_values[vm->instance_variable_count] = stored_value;
  vm->instance_variable_count++;
  bs_vm_trace_writer_set(vm,
                         target_instance_id,
                         variable_index,
                         -1,
                         false,
                         stored_value);
  return true;
}

static bs_vm_value bs_vm_instance_dynamic_array_get_or_zero(bs_vm *vm,
                                                            int32_t variable_index,
                                                            int32_t element_index,
                                                            int32_t target_instance_id) {
  bs_instance *instance = NULL;
  if (vm == NULL ||
      vm->runner == NULL ||
      target_instance_id < 0 ||
      variable_index < 0) {
    return bs_vm_value_zero();
  }

  instance = bs_game_runner_find_instance_by_id(vm->runner, target_instance_id);
  if (instance == NULL || instance->destroyed) {
    return bs_vm_value_zero();
  }

  for (size_t i = 0; i < vm->instance_array_count; i++) {
    if (vm->instance_array_instance_ids[i] == target_instance_id &&
        vm->instance_array_variable_indices[i] == variable_index &&
        vm->instance_array_element_indices[i] == element_index) {
      return vm->instance_array_values[i];
    }
  }
  return bs_vm_value_zero();
}

static bool bs_vm_instance_dynamic_array_set(bs_vm *vm,
                                             int32_t variable_index,
                                             int32_t element_index,
                                             int32_t target_instance_id,
                                             bs_vm_value value) {
  bs_instance *instance = NULL;
  bs_vm_value stored_value = bs_vm_value_zero();
  if (vm == NULL ||
      vm->runner == NULL ||
      target_instance_id < 0 ||
      variable_index < 0) {
    return false;
  }

  instance = bs_game_runner_find_instance_by_id(vm->runner, target_instance_id);
  if (instance == NULL || instance->destroyed) {
    return false;
  }

  if (!bs_vm_make_storable_value(vm, value, &stored_value)) {
    return false;
  }

  for (size_t i = 0; i < vm->instance_array_count; i++) {
    if (vm->instance_array_instance_ids[i] == target_instance_id &&
        vm->instance_array_variable_indices[i] == variable_index &&
        vm->instance_array_element_indices[i] == element_index) {
      vm->instance_array_values[i] = stored_value;
      bs_vm_trace_writer_set(vm,
                             target_instance_id,
                             variable_index,
                             element_index,
                             true,
                             stored_value);
      return true;
    }
  }

  if (vm->instance_array_count == vm->instance_array_capacity) {
    size_t new_capacity = (vm->instance_array_capacity == 0) ? 128u : (vm->instance_array_capacity * 2u);
    int32_t *grown_instance_ids = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    int32_t *grown_var_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    int32_t *grown_element_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    bs_vm_value *grown_values = (bs_vm_value *)malloc(new_capacity * sizeof(bs_vm_value));
    if (grown_instance_ids == NULL ||
        grown_var_indices == NULL ||
        grown_element_indices == NULL ||
        grown_values == NULL) {
      free(grown_instance_ids);
      free(grown_var_indices);
      free(grown_element_indices);
      free(grown_values);
      return false;
    }

    if (vm->instance_array_count > 0) {
      memcpy(grown_instance_ids,
             vm->instance_array_instance_ids,
             vm->instance_array_count * sizeof(int32_t));
      memcpy(grown_var_indices,
             vm->instance_array_variable_indices,
             vm->instance_array_count * sizeof(int32_t));
      memcpy(grown_element_indices,
             vm->instance_array_element_indices,
             vm->instance_array_count * sizeof(int32_t));
      memcpy(grown_values,
             vm->instance_array_values,
             vm->instance_array_count * sizeof(bs_vm_value));
    }

    free(vm->instance_array_instance_ids);
    free(vm->instance_array_variable_indices);
    free(vm->instance_array_element_indices);
    free(vm->instance_array_values);
    vm->instance_array_instance_ids = grown_instance_ids;
    vm->instance_array_variable_indices = grown_var_indices;
    vm->instance_array_element_indices = grown_element_indices;
    vm->instance_array_values = grown_values;
    vm->instance_array_capacity = new_capacity;
  }

  vm->instance_array_instance_ids[vm->instance_array_count] = target_instance_id;
  vm->instance_array_variable_indices[vm->instance_array_count] = variable_index;
  vm->instance_array_element_indices[vm->instance_array_count] = element_index;
  vm->instance_array_values[vm->instance_array_count] = stored_value;
  vm->instance_array_count++;
  bs_vm_trace_writer_set(vm,
                         target_instance_id,
                         variable_index,
                         element_index,
                         true,
                         stored_value);
  return true;
}

static bs_vm_value bs_vm_instance_get_for_id_or_zero(bs_vm *vm,
                                                     int32_t variable_index,
                                                     int32_t target_instance_id) {
  const char *name = NULL;
  bs_vm_value value = bs_vm_value_zero();
  if (vm == NULL ||
      vm->runner == NULL ||
      vm->game_data == NULL ||
      variable_index < 0 ||
      (size_t)variable_index >= vm->game_data->variable_count) {
    return bs_vm_value_zero();
  }

  name = bs_vm_variable_name(vm, variable_index);
  if (target_instance_id >= 0 && bs_vm_instance_variable_is_runner_managed(name)) {
    return bs_vm_value_number(
        bs_game_runner_instance_get_variable(vm->runner, target_instance_id, variable_index, name));
  }

  if (target_instance_id >= 0) {
    value = bs_vm_instance_dynamic_get_or_zero(vm, variable_index, target_instance_id);
    if (!(value.type == BS_VM_VALUE_NUMBER &&
          value.number == 0.0 &&
          !bs_vm_instance_has_scalar(vm, target_instance_id, variable_index))) {
      return value;
    }
  }
  return bs_vm_global_or_builtin_get_or_zero(vm, variable_index);
}

static bs_vm_value bs_vm_instance_get_array_or_zero(bs_vm *vm,
                                                    int32_t variable_index,
                                                    int32_t index,
                                                    int32_t target_instance_id) {
  bs_instance *instance = NULL;
  if (vm == NULL || vm->runner == NULL || target_instance_id < 0) {
    return bs_vm_value_zero();
  }

  if (bs_vm_variable_is_alarm(vm, variable_index)) {
    instance = bs_game_runner_find_instance_by_id(vm->runner, target_instance_id);
    if (instance == NULL || instance->destroyed) {
      return bs_vm_value_zero();
    }
    if (index < 0 || index >= 12) {
      return bs_vm_value_number(-1.0);
    }
    return bs_vm_value_number((double)instance->alarm[index]);
  }

  return bs_vm_instance_dynamic_array_get_or_zero(vm, variable_index, index, target_instance_id);
}

static bool bs_vm_instance_set_for_id(bs_vm *vm,
                                      int32_t variable_index,
                                      int32_t target_instance_id,
                                      bs_vm_value value) {
  const char *name = NULL;
  if (vm == NULL ||
      vm->runner == NULL ||
      vm->game_data == NULL ||
      target_instance_id < 0 ||
      variable_index < 0 ||
      (size_t)variable_index >= vm->game_data->variable_count) {
    return false;
  }

  name = bs_vm_variable_name(vm, variable_index);
  if (bs_vm_instance_variable_is_runner_managed(name)) {
    return bs_game_runner_instance_set_variable(vm->runner,
                                                target_instance_id,
                                                variable_index,
                                                name,
                                                bs_vm_value_to_number(value));
  }
  return bs_vm_instance_dynamic_set(vm, variable_index, target_instance_id, value);
}

static bool bs_vm_instance_set_for_target(bs_vm *vm,
                                          int32_t variable_index,
                                          int32_t target_instance,
                                          bs_vm_value value) {
  bool ok = true;
  if (vm == NULL || vm->runner == NULL) {
    return false;
  }

  if (target_instance == BS_INSTANCE_NOONE) {
    return true;
  }

  if (target_instance >= 0 && target_instance < 100000) {
    for (size_t i = 0; i < vm->runner->instance_count; i++) {
      bs_instance *inst = &vm->runner->instances[i];
      if (inst->destroyed) {
        continue;
      }
      if (bs_game_runner_object_is_child_of(vm->runner, inst->object_index, target_instance)) {
        ok = bs_vm_instance_set_for_id(vm, variable_index, inst->id, value) && ok;
      }
    }
    return ok;
  }

  return bs_vm_instance_set_for_id(vm,
                                   variable_index,
                                   bs_vm_resolve_single_instance_target(vm, target_instance),
                                   value);
}

static bool bs_vm_instance_set_array_for_target(bs_vm *vm,
                                                int32_t variable_index,
                                                int32_t index,
                                                int32_t target_instance,
                                                bs_vm_value value) {
  bs_instance *instance = NULL;
  bool ok = true;
  if (vm == NULL || vm->runner == NULL) {
    return false;
  }

  if (target_instance == BS_INSTANCE_NOONE) {
    return true;
  }

  if (bs_vm_variable_is_alarm(vm, variable_index)) {
    if (index < 0 || index >= 12) {
      return true;
    }

    if (target_instance >= 0 && target_instance < 100000) {
      for (size_t i = 0; i < vm->runner->instance_count; i++) {
        bs_instance *inst = &vm->runner->instances[i];
        if (inst->destroyed) {
          continue;
        }
        if (bs_game_runner_object_is_child_of(vm->runner, inst->object_index, target_instance)) {
          inst->alarm[index] = (int32_t)bs_vm_value_to_number(value);
        }
      }
      return true;
    }

    instance = bs_game_runner_find_instance_by_id(vm->runner,
                                                  bs_vm_resolve_single_instance_target(vm, target_instance));
    if (instance == NULL || instance->destroyed) {
      return true;
    }
    instance->alarm[index] = (int32_t)bs_vm_value_to_number(value);
    return true;
  }

  if (target_instance >= 0 && target_instance < 100000) {
    for (size_t i = 0; i < vm->runner->instance_count; i++) {
      bs_instance *inst = &vm->runner->instances[i];
      if (inst->destroyed) {
        continue;
      }
      if (bs_game_runner_object_is_child_of(vm->runner, inst->object_index, target_instance)) {
        ok = bs_vm_instance_dynamic_array_set(vm, variable_index, index, inst->id, value) && ok;
      }
    }
    return ok;
  }

  return bs_vm_instance_dynamic_array_set(vm,
                                          variable_index,
                                          index,
                                          bs_vm_resolve_single_instance_target(vm, target_instance),
                                          value);
}

static bool bs_vm_global_set(bs_vm *vm, int32_t variable_index, bs_vm_value value) {
  const char *name = NULL;
  bs_vm_value stored_value = bs_vm_value_zero();
  if (vm == NULL || variable_index < 0) {
    return false;
  }

  name = bs_vm_variable_name(vm, variable_index);
  if (name != NULL &&
      bs_vm_trace_writer_enabled() &&
      (strcmp(name, "msc") == 0 || strcmp(name, "msg") == 0)) {
    if (value.type == BS_VM_VALUE_STRING) {
      printf("  [GLOBAL SET] %s=\"%s\"\n", name, value.string != NULL ? value.string : "");
    } else {
      printf("  [GLOBAL SET] %s=%.3f\n", name, value.number);
    }
  }
  if (name != NULL &&
      vm->runner != NULL &&
      strcmp(name, "room_persistent") == 0) {
    if (vm->runner->current_room_index >= 0 &&
        (size_t)vm->runner->current_room_index < vm->runner->room_persistent_flag_count &&
        vm->runner->room_persistent_flags != NULL) {
      vm->runner->room_persistent_flags[(size_t)vm->runner->current_room_index] =
          (bs_vm_value_to_number(value) != 0.0);
    }
    return true;
  }

  if (!bs_vm_make_storable_value(vm, value, &stored_value)) {
    return false;
  }

  for (size_t i = 0; i < vm->global_variable_count; i++) {
    if (vm->global_variable_indices[i] == variable_index) {
      vm->global_variable_values[i] = stored_value;
      return true;
    }
  }

  if (vm->global_variable_count == vm->global_variable_capacity) {
    size_t new_capacity = (vm->global_variable_capacity == 0) ? 128u : (vm->global_variable_capacity * 2u);
    int32_t *grown_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    bs_vm_value *grown_values = (bs_vm_value *)malloc(new_capacity * sizeof(bs_vm_value));
    if (grown_indices == NULL || grown_values == NULL) {
      free(grown_indices);
      free(grown_values);
      return false;
    }

    if (vm->global_variable_count > 0) {
      memcpy(grown_indices,
             vm->global_variable_indices,
             vm->global_variable_count * sizeof(int32_t));
      memcpy(grown_values,
             vm->global_variable_values,
             vm->global_variable_count * sizeof(bs_vm_value));
    }

    free(vm->global_variable_indices);
    free(vm->global_variable_values);
    vm->global_variable_indices = grown_indices;
    vm->global_variable_values = grown_values;
    vm->global_variable_capacity = new_capacity;
  }

  vm->global_variable_indices[vm->global_variable_count] = variable_index;
  vm->global_variable_values[vm->global_variable_count] = stored_value;
  vm->global_variable_count++;
  return true;
}

static bs_vm_value bs_vm_global_get_or_zero(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL || variable_index < 0) {
    return bs_vm_value_zero();
  }

  for (size_t i = 0; i < vm->global_variable_count; i++) {
    if (vm->global_variable_indices[i] == variable_index) {
      return vm->global_variable_values[i];
    }
  }

  return bs_vm_value_zero();
}

static bs_vm_value bs_vm_global_array_get_or_zero(const bs_vm *vm,
                                                  int32_t variable_index,
                                                  int32_t element_index) {
  if (vm == NULL || variable_index < 0) {
    return bs_vm_value_zero();
  }

  for (size_t i = 0; i < vm->global_array_count; i++) {
    if (vm->global_array_variable_indices[i] == variable_index &&
        vm->global_array_element_indices[i] == element_index) {
      return vm->global_array_values[i];
    }
  }

  return bs_vm_value_zero();
}

static bool bs_vm_global_array_set(bs_vm *vm,
                                   int32_t variable_index,
                                   int32_t element_index,
                                   bs_vm_value value) {
  bs_vm_value stored_value = bs_vm_value_zero();
  const char *name = NULL;
  if (vm == NULL || variable_index < 0) {
    return false;
  }

  name = bs_vm_variable_name(vm, variable_index);
  if (name != NULL &&
      bs_vm_trace_writer_enabled() &&
      (strcmp(name, "mystring") == 0 || strcmp(name, "msg") == 0 || strcmp(name, "textstring") == 0)) {
    if (value.type == BS_VM_VALUE_STRING) {
      printf("  [GLOBAL ARRAY SET] %s[%d]=\"%s\"\n",
             name,
             element_index,
             value.string != NULL ? value.string : "");
    } else {
      printf("  [GLOBAL ARRAY SET] %s[%d]=%.3f\n", name, element_index, value.number);
    }
  }

  if (!bs_vm_make_storable_value(vm, value, &stored_value)) {
    return false;
  }

  for (size_t i = 0; i < vm->global_array_count; i++) {
    if (vm->global_array_variable_indices[i] == variable_index &&
        vm->global_array_element_indices[i] == element_index) {
      vm->global_array_values[i] = stored_value;
      return true;
    }
  }

  if (vm->global_array_count == vm->global_array_capacity) {
    size_t new_capacity = (vm->global_array_capacity == 0) ? 128u : (vm->global_array_capacity * 2u);
    int32_t *grown_var_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    int32_t *grown_elem_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    bs_vm_value *grown_values = (bs_vm_value *)malloc(new_capacity * sizeof(bs_vm_value));
    if (grown_var_indices == NULL || grown_elem_indices == NULL || grown_values == NULL) {
      free(grown_var_indices);
      free(grown_elem_indices);
      free(grown_values);
      return false;
    }

    if (vm->global_array_count > 0) {
      memcpy(grown_var_indices,
             vm->global_array_variable_indices,
             vm->global_array_count * sizeof(int32_t));
      memcpy(grown_elem_indices,
             vm->global_array_element_indices,
             vm->global_array_count * sizeof(int32_t));
      memcpy(grown_values,
             vm->global_array_values,
             vm->global_array_count * sizeof(bs_vm_value));
    }

    free(vm->global_array_variable_indices);
    free(vm->global_array_element_indices);
    free(vm->global_array_values);
    vm->global_array_variable_indices = grown_var_indices;
    vm->global_array_element_indices = grown_elem_indices;
    vm->global_array_values = grown_values;
    vm->global_array_capacity = new_capacity;
  }

  vm->global_array_variable_indices[vm->global_array_count] = variable_index;
  vm->global_array_element_indices[vm->global_array_count] = element_index;
  vm->global_array_values[vm->global_array_count] = stored_value;
  vm->global_array_count++;
  return true;
}

static bool bs_vm_global_has_scalar(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL || variable_index < 0) {
    return false;
  }
  for (size_t i = 0; i < vm->global_variable_count; i++) {
    if (vm->global_variable_indices[i] == variable_index) {
      return true;
    }
  }
  return false;
}

static bool bs_vm_global_has_array(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL || variable_index < 0) {
    return false;
  }
  for (size_t i = 0; i < vm->global_array_count; i++) {
    if (vm->global_array_variable_indices[i] == variable_index) {
      return true;
    }
  }
  return false;
}

static void bs_vm_global_array_clear_variable(bs_vm *vm, int32_t variable_index) {
  size_t write = 0;
  if (vm == NULL || variable_index < 0) {
    return;
  }
  for (size_t i = 0; i < vm->global_array_count; i++) {
    if (vm->global_array_variable_indices[i] == variable_index) {
      continue;
    }
    if (write != i) {
      vm->global_array_variable_indices[write] = vm->global_array_variable_indices[i];
      vm->global_array_element_indices[write] = vm->global_array_element_indices[i];
      vm->global_array_values[write] = vm->global_array_values[i];
    }
    write++;
  }
  vm->global_array_count = write;
}

static bool bs_vm_instance_has_scalar(const bs_vm *vm, int32_t instance_id, int32_t variable_index) {
  if (vm == NULL || instance_id < 0 || variable_index < 0) {
    return false;
  }
  for (size_t i = 0; i < vm->instance_variable_count; i++) {
    if (vm->instance_variable_instance_ids[i] == instance_id &&
        vm->instance_variable_indices[i] == variable_index) {
      return true;
    }
  }
  return false;
}

static bool bs_vm_instance_has_array(const bs_vm *vm, int32_t instance_id, int32_t variable_index) {
  if (vm == NULL || instance_id < 0 || variable_index < 0) {
    return false;
  }
  for (size_t i = 0; i < vm->instance_array_count; i++) {
    if (vm->instance_array_instance_ids[i] == instance_id &&
        vm->instance_array_variable_indices[i] == variable_index) {
      return true;
    }
  }
  return false;
}

static void bs_vm_instance_array_clear_variable(bs_vm *vm, int32_t instance_id, int32_t variable_index) {
  size_t write = 0;
  if (vm == NULL || instance_id < 0 || variable_index < 0) {
    return;
  }
  for (size_t i = 0; i < vm->instance_array_count; i++) {
    if (vm->instance_array_instance_ids[i] == instance_id &&
        vm->instance_array_variable_indices[i] == variable_index) {
      continue;
    }
    if (write != i) {
      vm->instance_array_instance_ids[write] = vm->instance_array_instance_ids[i];
      vm->instance_array_variable_indices[write] = vm->instance_array_variable_indices[i];
      vm->instance_array_element_indices[write] = vm->instance_array_element_indices[i];
      vm->instance_array_values[write] = vm->instance_array_values[i];
    }
    write++;
  }
  vm->instance_array_count = write;
}

static bool bs_vm_copy_array_variable(bs_vm *vm,
                                      bs_vm_locals *locals,
                                      bs_vm_array_scope src_scope,
                                      int32_t src_instance_id,
                                      int32_t src_variable_index,
                                      bs_vm_array_scope dst_scope,
                                      int32_t dst_instance_id,
                                      int32_t dst_variable_index) {
  bool ok = true;
  if (vm == NULL || src_variable_index < 0 || dst_variable_index < 0) {
    return false;
  }

  if (dst_scope == BS_VM_ARRAY_SCOPE_LOCAL) {
    bs_vm_locals_array_clear_variable(locals, dst_variable_index);
  } else if (dst_scope == BS_VM_ARRAY_SCOPE_GLOBAL) {
    bs_vm_global_array_clear_variable(vm, dst_variable_index);
  } else if (dst_scope == BS_VM_ARRAY_SCOPE_INSTANCE) {
    bs_vm_instance_array_clear_variable(vm, dst_instance_id, dst_variable_index);
  } else {
    return false;
  }

  if (src_scope == BS_VM_ARRAY_SCOPE_LOCAL) {
    if (locals == NULL) {
      return false;
    }
    for (size_t i = 0; i < locals->array_count; i++) {
      if (locals->array_variable_indices[i] != src_variable_index) {
        continue;
      }
      if (dst_scope == BS_VM_ARRAY_SCOPE_LOCAL) {
        ok = bs_vm_locals_array_set(vm,
                                    locals,
                                    dst_variable_index,
                                    locals->array_element_indices[i],
                                    locals->array_values[i]) && ok;
      } else if (dst_scope == BS_VM_ARRAY_SCOPE_GLOBAL) {
        ok = bs_vm_global_array_set(vm,
                                    dst_variable_index,
                                    locals->array_element_indices[i],
                                    locals->array_values[i]) && ok;
      } else {
        ok = bs_vm_instance_dynamic_array_set(vm,
                                              dst_variable_index,
                                              locals->array_element_indices[i],
                                              dst_instance_id,
                                              locals->array_values[i]) && ok;
      }
    }
    return ok;
  }

  if (src_scope == BS_VM_ARRAY_SCOPE_GLOBAL) {
    for (size_t i = 0; i < vm->global_array_count; i++) {
      if (vm->global_array_variable_indices[i] != src_variable_index) {
        continue;
      }
      if (dst_scope == BS_VM_ARRAY_SCOPE_LOCAL) {
        ok = bs_vm_locals_array_set(vm,
                                    locals,
                                    dst_variable_index,
                                    vm->global_array_element_indices[i],
                                    vm->global_array_values[i]) && ok;
      } else if (dst_scope == BS_VM_ARRAY_SCOPE_GLOBAL) {
        ok = bs_vm_global_array_set(vm,
                                    dst_variable_index,
                                    vm->global_array_element_indices[i],
                                    vm->global_array_values[i]) && ok;
      } else {
        ok = bs_vm_instance_dynamic_array_set(vm,
                                              dst_variable_index,
                                              vm->global_array_element_indices[i],
                                              dst_instance_id,
                                              vm->global_array_values[i]) && ok;
      }
    }
    return ok;
  }

  if (src_scope == BS_VM_ARRAY_SCOPE_INSTANCE) {
    for (size_t i = 0; i < vm->instance_array_count; i++) {
      if (vm->instance_array_instance_ids[i] != src_instance_id ||
          vm->instance_array_variable_indices[i] != src_variable_index) {
        continue;
      }
      if (dst_scope == BS_VM_ARRAY_SCOPE_LOCAL) {
        ok = bs_vm_locals_array_set(vm,
                                    locals,
                                    dst_variable_index,
                                    vm->instance_array_element_indices[i],
                                    vm->instance_array_values[i]) && ok;
      } else if (dst_scope == BS_VM_ARRAY_SCOPE_GLOBAL) {
        ok = bs_vm_global_array_set(vm,
                                    dst_variable_index,
                                    vm->instance_array_element_indices[i],
                                    vm->instance_array_values[i]) && ok;
      } else {
        ok = bs_vm_instance_dynamic_array_set(vm,
                                              dst_variable_index,
                                              vm->instance_array_element_indices[i],
                                              dst_instance_id,
                                              vm->instance_array_values[i]) && ok;
      }
    }
    return ok;
  }

  return false;
}

static bs_vm_builtin_callback bs_vm_find_builtin(const bs_vm *vm, const char *name) {
  if (vm == NULL || name == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < vm->builtin_count; i++) {
    if (vm->builtin_names[i] != NULL && strcmp(vm->builtin_names[i], name) == 0) {
      return vm->builtin_callbacks[i];
    }
  }

  return NULL;
}

bool bs_vm_register_builtin(bs_vm *vm, const char *name, bs_vm_builtin_callback callback) {
  if (vm == NULL || name == NULL || callback == NULL) {
    return false;
  }

  for (size_t i = 0; i < vm->builtin_count; i++) {
    if (vm->builtin_names[i] != NULL && strcmp(vm->builtin_names[i], name) == 0) {
      vm->builtin_callbacks[i] = callback;
      return true;
    }
  }

  if (vm->builtin_count == vm->builtin_capacity) {
    size_t new_capacity = (vm->builtin_capacity == 0) ? 64u : (vm->builtin_capacity * 2u);
    char **grown_names = (char **)malloc(new_capacity * sizeof(char *));
    bs_vm_builtin_callback *grown_callbacks =
        (bs_vm_builtin_callback *)malloc(new_capacity * sizeof(bs_vm_builtin_callback));
    if (grown_names == NULL || grown_callbacks == NULL) {
      free(grown_names);
      free(grown_callbacks);
      return false;
    }

    if (vm->builtin_count > 0) {
      memcpy(grown_names, vm->builtin_names, vm->builtin_count * sizeof(char *));
      memcpy(grown_callbacks,
             vm->builtin_callbacks,
             vm->builtin_count * sizeof(bs_vm_builtin_callback));
    }

    free(vm->builtin_names);
    free(vm->builtin_callbacks);
    vm->builtin_names = grown_names;
    vm->builtin_callbacks = grown_callbacks;
    vm->builtin_capacity = new_capacity;
  }

  vm->builtin_names[vm->builtin_count] = bs_vm_dup_cstr(name);
  if (vm->builtin_names[vm->builtin_count] == NULL) {
    return false;
  }
  vm->builtin_callbacks[vm->builtin_count] = callback;
  vm->builtin_count++;
  return true;
}

static int32_t bs_vm_find_script_code_id(const bs_game_data *game_data, const char *function_name) {
  if (game_data == NULL || function_name == NULL) {
    return -1;
  }

  for (size_t i = 0; i < game_data->script_count; i++) {
    const bs_script_data *script = &game_data->scripts[i];
    if (script->name != NULL && strcmp(script->name, function_name) == 0) {
      return script->code_id;
    }
  }

  return -1;
}

static int bs_vm_compare_values(bs_vm_value lhs, bs_vm_value rhs) {
  if (lhs.type == BS_VM_VALUE_STRING && rhs.type == BS_VM_VALUE_STRING) {
    const char *lhs_s = (lhs.string != NULL) ? lhs.string : "";
    const char *rhs_s = (rhs.string != NULL) ? rhs.string : "";
    return strcmp(lhs_s, rhs_s);
  }

  {
    double a = bs_vm_value_to_number(lhs);
    double b = bs_vm_value_to_number(rhs);
    if (a < b) {
      return -1;
    }
    if (a > b) {
      return 1;
    }
    return 0;
  }
}

static bool bs_vm_compare_bool(int cmp, uint8_t comparison_type) {
  switch (comparison_type) {
    case BS_COMPARISON_LT:
      return cmp < 0;
    case BS_COMPARISON_LTE:
      return cmp <= 0;
    case BS_COMPARISON_EQ:
      return cmp == 0;
    case BS_COMPARISON_NEQ:
      return cmp != 0;
    case BS_COMPARISON_GTE:
      return cmp >= 0;
    case BS_COMPARISON_GT:
      return cmp > 0;
    default:
      return false;
  }
}

static int32_t bs_vm_branch_offset(uint32_t raw_operand) {
  uint32_t raw = raw_operand & 0x7FFFFFu;
  if ((raw & 0x400000u) != 0u) {
    raw |= 0xFF800000u;
  }
  return (int32_t)raw;
}

static bool bs_vm_find_branch_target(const bs_decoded_code *decoded,
                                     size_t current_instruction_index,
                                     int32_t branch_offset,
                                     size_t *out_instruction_index) {
  int64_t current_offset = 0;
  int64_t target_offset = 0;
  int32_t found = -1;

  if (decoded == NULL ||
      out_instruction_index == NULL ||
      current_instruction_index >= decoded->instruction_count) {
    return false;
  }

  current_offset = (int64_t)decoded->instruction_offsets[current_instruction_index];
  target_offset = current_offset + ((int64_t)branch_offset * 4ll);
  if (target_offset < 0 || target_offset > (int64_t)UINT_MAX) {
    return false;
  }

  found = bs_decoded_lookup_instruction_index(decoded, (uint32_t)target_offset);
  if (found < 0) {
    return false;
  }

  *out_instruction_index = (size_t)found;
  return true;
}

static bool bs_vm_collect_target_instance_ids(bs_vm *vm,
                                              int32_t target_id,
                                              int32_t **out_ids,
                                              size_t *out_count) {
  size_t count = 0;
  int32_t *ids = NULL;
  if (out_ids == NULL || out_count == NULL) {
    return false;
  }
  *out_ids = NULL;
  *out_count = 0;

  if (vm == NULL || vm->runner == NULL || vm->runner->game_data == NULL) {
    return true;
  }

  if (target_id >= 100000) {
    bs_instance *inst = bs_game_runner_find_instance_by_id(vm->runner, target_id);
    if (inst != NULL && !inst->destroyed) {
      ids = (int32_t *)malloc(sizeof(int32_t));
      if (ids == NULL) {
        return false;
      }
      ids[0] = inst->id;
      count = 1;
    }
  } else {
    for (size_t i = 0; i < vm->runner->instance_count; i++) {
      const bs_instance *inst = &vm->runner->instances[i];
      if (inst->destroyed) {
        continue;
      }
      if (!bs_game_runner_object_is_child_of(vm->runner, inst->object_index, target_id)) {
        continue;
      }
      count++;
    }

    if (count > 0) {
      size_t write = 0;
      ids = (int32_t *)malloc(count * sizeof(int32_t));
      if (ids == NULL) {
        return false;
      }
      for (size_t i = 0; i < vm->runner->instance_count; i++) {
        const bs_instance *inst = &vm->runner->instances[i];
        if (inst->destroyed) {
          continue;
        }
        if (!bs_game_runner_object_is_child_of(vm->runner, inst->object_index, target_id)) {
          continue;
        }
        ids[write] = inst->id;
        write++;
      }
    }
  }

  *out_ids = ids;
  *out_count = count;
  return true;
}

static bool bs_vm_find_next_alive_instance_id(bs_vm *vm,
                                              const int32_t *instance_ids,
                                              size_t instance_count,
                                              size_t start_index,
                                              size_t *out_index,
                                              int32_t *out_instance_id) {
  if (vm == NULL ||
      vm->runner == NULL ||
      instance_ids == NULL ||
      out_index == NULL ||
      out_instance_id == NULL) {
    return false;
  }

  for (size_t i = start_index; i < instance_count; i++) {
    bs_instance *inst = bs_game_runner_find_instance_by_id(vm->runner, instance_ids[i]);
    if (inst != NULL && !inst->destroyed) {
      *out_index = i;
      *out_instance_id = inst->id;
      return true;
    }
  }

  return false;
}

static bool bs_vm_push_binary_numeric(bs_vm_stack *stack, double value) {
  return bs_vm_stack_push(stack, bs_vm_value_number(value));
}

static bool bs_vm_binary_real_op(bs_vm *vm, bs_vm_stack *stack, uint8_t opcode) {
  bs_vm_value rhs = bs_vm_stack_pop_or_zero(stack);
  bs_vm_value lhs = bs_vm_stack_pop_or_zero(stack);
  double a = bs_vm_value_to_number(lhs);
  double b = bs_vm_value_to_number(rhs);

  switch (opcode) {
    case BS_OPCODE_MUL:
      return bs_vm_push_binary_numeric(stack, a * b);
    case BS_OPCODE_DIV:
      return bs_vm_push_binary_numeric(stack, (b == 0.0) ? 0.0 : (a / b));
    case BS_OPCODE_ADD: {
      if (lhs.type == BS_VM_VALUE_STRING || rhs.type == BS_VM_VALUE_STRING) {
        char lhs_scratch[64];
        char rhs_scratch[64];
        const char *lhs_s = NULL;
        const char *rhs_s = NULL;
        char combined[2048];
        bs_vm_value combined_value = bs_vm_value_zero();
        bs_vm_value stored_value = bs_vm_value_zero();
        if (vm == NULL) {
          return false;
        }
        lhs_s = (lhs.type == BS_VM_VALUE_STRING)
                    ? (lhs.string != NULL ? lhs.string : "")
                    : ((void)snprintf(lhs_scratch, sizeof(lhs_scratch), "%g", lhs.number), lhs_scratch);
        rhs_s = (rhs.type == BS_VM_VALUE_STRING)
                    ? (rhs.string != NULL ? rhs.string : "")
                    : ((void)snprintf(rhs_scratch, sizeof(rhs_scratch), "%g", rhs.number), rhs_scratch);
        (void)snprintf(combined, sizeof(combined), "%s%s", lhs_s, rhs_s);
        combined_value = bs_vm_value_string(combined);
        if (!bs_vm_make_storable_value(vm, combined_value, &stored_value)) {
          return false;
        }
        return bs_vm_stack_push(stack, stored_value);
      }
      return bs_vm_push_binary_numeric(stack, a + b);
    }
    case BS_OPCODE_SUB:
      return bs_vm_push_binary_numeric(stack, a - b);
    default:
      return false;
  }
}

static bool bs_vm_binary_int_op(bs_vm_stack *stack, uint8_t opcode) {
  bs_vm_value rhs = bs_vm_stack_pop_or_zero(stack);
  bs_vm_value lhs = bs_vm_stack_pop_or_zero(stack);
  int64_t a = bs_vm_value_to_int64(lhs);
  int64_t b = bs_vm_value_to_int64(rhs);
  int64_t result = 0;

  switch (opcode) {
    case BS_OPCODE_REM:
    case BS_OPCODE_MOD:
      result = (b == 0) ? 0 : (a % b);
      break;
    case BS_OPCODE_AND:
      result = a & b;
      break;
    case BS_OPCODE_OR:
      result = a | b;
      break;
    case BS_OPCODE_XOR:
      result = a ^ b;
      break;
    case BS_OPCODE_SHL: {
      uint32_t shift = ((uint32_t)b) & 63u;
      result = (int64_t)(((uint64_t)a) << shift);
      break;
    }
    case BS_OPCODE_SHR: {
      uint32_t shift = ((uint32_t)b) & 63u;
      result = (a >> shift);
      break;
    }
    default:
      return false;
  }

  return bs_vm_push_binary_numeric(stack, (double)result);
}

static void bs_decoded_code_free(bs_decoded_code *decoded) {
  if (decoded == NULL) {
    return;
  }

  free(decoded->instructions);
  free(decoded->instruction_offsets);
  decoded->instructions = NULL;
  decoded->instruction_offsets = NULL;
  decoded->instruction_count = 0;
}

static bool bs_can_read(uint32_t total_size, uint32_t offset, size_t need) {
  if (offset > total_size) {
    return false;
  }
  return need <= (size_t)(total_size - offset);
}

static bool bs_read_i16_le_bytes(const uint8_t *bytes, uint32_t length, uint32_t offset, int16_t *out) {
  uint16_t raw = 0;
  if (bytes == NULL || out == NULL || !bs_can_read(length, offset, 2)) {
    return false;
  }

  raw = (uint16_t)((uint16_t)bytes[offset] | ((uint16_t)bytes[offset + 1] << 8));
  *out = (int16_t)raw;
  return true;
}

static bool bs_read_i32_le_bytes(const uint8_t *bytes, uint32_t length, uint32_t offset, int32_t *out) {
  uint32_t raw = 0;
  if (bytes == NULL || out == NULL || !bs_can_read(length, offset, 4)) {
    return false;
  }

  raw = ((uint32_t)bytes[offset]) |
        ((uint32_t)bytes[offset + 1] << 8) |
        ((uint32_t)bytes[offset + 2] << 16) |
        ((uint32_t)bytes[offset + 3] << 24);
  *out = (int32_t)raw;
  return true;
}

static bool bs_read_i64_le_bytes(const uint8_t *bytes, uint32_t length, uint32_t offset, int64_t *out) {
  int32_t lo = 0;
  int32_t hi = 0;
  uint64_t lo_u64 = 0;
  uint64_t hi_u64 = 0;
  if (out == NULL ||
      !bs_read_i32_le_bytes(bytes, length, offset, &lo) ||
      !bs_read_i32_le_bytes(bytes, length, offset + 4, &hi)) {
    return false;
  }

  lo_u64 = ((uint64_t)(uint32_t)lo);
  hi_u64 = ((uint64_t)(uint32_t)hi) << 32;
  *out = (int64_t)(lo_u64 | hi_u64);
  return true;
}

static bool bs_read_f32_le_bytes(const uint8_t *bytes, uint32_t length, uint32_t offset, float *out) {
  int32_t bits = 0;
  if (out == NULL || !bs_read_i32_le_bytes(bytes, length, offset, &bits)) {
    return false;
  }
  memcpy(out, &bits, sizeof(bits));
  return true;
}

static bool bs_read_f64_le_bytes(const uint8_t *bytes, uint32_t length, uint32_t offset, double *out) {
  int64_t bits = 0;
  if (out == NULL || !bs_read_i64_le_bytes(bytes, length, offset, &bits)) {
    return false;
  }
  memcpy(out, &bits, sizeof(bits));
  return true;
}

static bool bs_decode_bytecode(const bs_code_entry_data *entry, bs_decoded_code *out_decoded) {
  const uint8_t *bytecode = NULL;
  uint32_t bytecode_length = 0;
  size_t max_instruction_count = 0;
  bs_instruction *instructions = NULL;
  uint32_t *instruction_offsets = NULL;
  uint32_t pos = 0;
  size_t instruction_count = 0;

  if (entry == NULL || out_decoded == NULL) {
    return false;
  }

  memset(out_decoded, 0, sizeof(*out_decoded));

  if (entry->bytecode == NULL || entry->bytecode_length == 0) {
    return true;
  }

  bytecode = entry->bytecode;
  bytecode_length = entry->bytecode_length;
  max_instruction_count = ((size_t)bytecode_length + 3u) / 4u;

  instructions = (bs_instruction *)calloc(max_instruction_count, sizeof(bs_instruction));
  instruction_offsets = (uint32_t *)calloc(max_instruction_count, sizeof(uint32_t));
  if (instructions == NULL || instruction_offsets == NULL) {
    free(instructions);
    free(instruction_offsets);
    return false;
  }

  while (pos < bytecode_length) {
    int32_t word = 0;
    bs_instruction instr = {0};
    uint8_t opcode = 0;
    uint8_t type1 = 0;
    uint8_t type2 = 0;
    int16_t extra = 0;
    uint32_t operand24 = 0;

    if (instruction_count >= max_instruction_count) {
      free(instructions);
      free(instruction_offsets);
      return false;
    }
    if (!bs_read_i32_le_bytes(bytecode, bytecode_length, pos, &word)) {
      free(instructions);
      free(instruction_offsets);
      return false;
    }
    if (!bs_read_i16_le_bytes(bytecode, bytecode_length, pos, &extra)) {
      free(instructions);
      free(instruction_offsets);
      return false;
    }

    opcode = (uint8_t)(((uint32_t)word >> 24) & 0xFFu);
    type1 = (uint8_t)(((uint32_t)word >> 16) & 0x0Fu);
    type2 = (uint8_t)(((uint32_t)word >> 20) & 0x0Fu);
    operand24 = ((uint32_t)word & 0x00FFFFFFu);

    instr.opcode = opcode;
    instr.type1 = type1;
    instr.type2 = type2;
    instr.extra = extra;
    instr.raw_operand = operand24;
    instr.variable_index = -1;
    instr.variable_type = 0;
    instr.function_index = -1;
    instr.string_index = -1;

    instruction_offsets[instruction_count] = pos;
    pos += 4;

    switch (opcode) {
      case BS_OPCODE_PUSH: {
        switch (type1) {
          case BS_DATA_TYPE_DOUBLE:
            if (!bs_read_f64_le_bytes(bytecode, bytecode_length, pos, &instr.double_value)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            pos += 8;
            break;
          case BS_DATA_TYPE_FLOAT:
            if (!bs_read_f32_le_bytes(bytecode, bytecode_length, pos, &instr.float_value)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            pos += 4;
            break;
          case BS_DATA_TYPE_INT32:
            if (!bs_read_i32_le_bytes(bytecode, bytecode_length, pos, &instr.int_value)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            pos += 4;
            break;
          case BS_DATA_TYPE_INT64:
            if (!bs_read_i64_le_bytes(bytecode, bytecode_length, pos, &instr.long_value)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            pos += 8;
            break;
          case BS_DATA_TYPE_BOOLEAN:
            if (!bs_read_i32_le_bytes(bytecode, bytecode_length, pos, &instr.int_value)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            pos += 4;
            break;
          case BS_DATA_TYPE_STRING:
            if (!bs_read_i32_le_bytes(bytecode, bytecode_length, pos, &instr.string_index)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            pos += 4;
            break;
          case BS_DATA_TYPE_INT16:
            instr.int_value = (int32_t)extra;
            break;
          case BS_DATA_TYPE_VARIABLE: {
            int32_t ref_value = 0;
            if (!bs_read_i32_le_bytes(bytecode, bytecode_length, pos, &ref_value)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            instr.variable_type = (int32_t)(((uint32_t)ref_value >> 24) & 0xF8u);
            pos += 4;
            break;
          }
          default:
            if (!bs_read_i32_le_bytes(bytecode, bytecode_length, pos, &instr.int_value)) {
              free(instructions);
              free(instruction_offsets);
              return false;
            }
            pos += 4;
            break;
        }
        break;
      }
      case BS_OPCODE_PUSHLOC:
      case BS_OPCODE_PUSHGLB:
      case BS_OPCODE_PUSHBLTN:
      case BS_OPCODE_POP: {
        int32_t ref_value = 0;
        if (!bs_read_i32_le_bytes(bytecode, bytecode_length, pos, &ref_value)) {
          free(instructions);
          free(instruction_offsets);
          return false;
        }
        instr.variable_type = (int32_t)(((uint32_t)ref_value >> 24) & 0xF8u);
        pos += 4;
        break;
      }
      case BS_OPCODE_CALL:
        if (!bs_can_read(bytecode_length, pos, 4)) {
          free(instructions);
          free(instruction_offsets);
          return false;
        }
        pos += 4;
        break;
      case BS_OPCODE_PUSHI:
        instr.int_value = (int32_t)extra;
        break;
      default:
        break;
    }

    instructions[instruction_count] = instr;
    instruction_count++;
  }

  out_decoded->instructions = instructions;
  out_decoded->instruction_offsets = instruction_offsets;
  out_decoded->instruction_count = instruction_count;

  if (instruction_count < max_instruction_count) {
    if (instruction_count == 0) {
      free(out_decoded->instructions);
      free(out_decoded->instruction_offsets);
      out_decoded->instructions = NULL;
      out_decoded->instruction_offsets = NULL;
    } else {
      bs_instruction *shrunk_instructions =
          (bs_instruction *)realloc(out_decoded->instructions, instruction_count * sizeof(bs_instruction));
      uint32_t *shrunk_offsets =
          (uint32_t *)realloc(out_decoded->instruction_offsets, instruction_count * sizeof(uint32_t));
      if (shrunk_instructions != NULL) {
        out_decoded->instructions = shrunk_instructions;
      }
      if (shrunk_offsets != NULL) {
        out_decoded->instruction_offsets = shrunk_offsets;
      }
    }
  }

  return true;
}

static int32_t bs_decoded_lookup_instruction_index(const bs_decoded_code *decoded, uint32_t local_offset) {
  size_t lo = 0;
  size_t hi = 0;
  if (decoded == NULL || decoded->instruction_count == 0 || decoded->instruction_offsets == NULL) {
    return -1;
  }

  hi = decoded->instruction_count;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) / 2u);
    uint32_t mid_offset = decoded->instruction_offsets[mid];
    if (local_offset < mid_offset) {
      hi = mid;
    } else if (local_offset > mid_offset) {
      lo = mid + 1u;
    } else {
      return (int32_t)mid;
    }
  }

  return -1;
}

static int bs_code_range_compare(const void *a, const void *b) {
  const bs_code_range *lhs = (const bs_code_range *)a;
  const bs_code_range *rhs = (const bs_code_range *)b;
  if (lhs->start < rhs->start) {
    return -1;
  }
  if (lhs->start > rhs->start) {
    return 1;
  }
  return 0;
}

static bool bs_build_code_ranges(bs_vm *vm) {
  size_t range_count = 0;
  bs_code_range *ranges = NULL;
  if (vm == NULL || vm->game_data == NULL) {
    return false;
  }

  for (size_t i = 0; i < vm->game_data->code_entry_count; i++) {
    if (vm->game_data->code_entries[i].bytecode_length > 0) {
      range_count++;
    }
  }

  if (range_count == 0) {
    vm->code_ranges = NULL;
    vm->code_range_count = 0;
    return true;
  }

  ranges = (bs_code_range *)calloc(range_count, sizeof(bs_code_range));
  if (ranges == NULL) {
    return false;
  }

  {
    size_t out_idx = 0;
    for (size_t i = 0; i < vm->game_data->code_entry_count; i++) {
      const bs_code_entry_data *entry = &vm->game_data->code_entries[i];
      if (entry->bytecode_length == 0) {
        continue;
      }

      {
        uint64_t start_u64 = (uint64_t)entry->bytecode_absolute_offset;
        uint64_t end_u64 = start_u64 + (uint64_t)entry->bytecode_length;
        if (end_u64 > (uint64_t)UINT_MAX) {
          free(ranges);
          return false;
        }
        ranges[out_idx].start = entry->bytecode_absolute_offset;
        ranges[out_idx].end = (uint32_t)end_u64;
        ranges[out_idx].code_entry_index = i;
        out_idx++;
      }
    }
  }

  qsort(ranges, range_count, sizeof(bs_code_range), bs_code_range_compare);
  vm->code_ranges = ranges;
  vm->code_range_count = range_count;
  return true;
}

static const bs_code_range *bs_find_code_range(const bs_vm *vm, uint32_t absolute_offset) {
  size_t lo = 0;
  size_t hi = 0;
  if (vm == NULL || vm->code_ranges == NULL || vm->code_range_count == 0) {
    return NULL;
  }

  hi = vm->code_range_count;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) / 2u);
    const bs_code_range *range = &vm->code_ranges[mid];
    if (absolute_offset < range->start) {
      hi = mid;
    } else if (absolute_offset >= range->end) {
      lo = mid + 1u;
    } else {
      return range;
    }
  }

  return NULL;
}

static uint32_t bs_resolve_variable_chains(bs_vm *vm) {
  uint32_t resolved = 0;
  if (vm == NULL || vm->game_data == NULL) {
    return 0;
  }

  for (size_t var_idx = 0; var_idx < vm->game_data->variable_count; var_idx++) {
    const bs_variable_data *variable = &vm->game_data->variables[var_idx];
    int32_t occ_count = variable->occurrence_count;
    uint32_t instr_addr = 0;

    if (occ_count <= 0 || variable->first_occurrence_offset < 0) {
      continue;
    }

    instr_addr = (uint32_t)variable->first_occurrence_offset;
    for (int32_t occ_i = 0; occ_i < occ_count; occ_i++) {
      const bs_code_range *range = bs_find_code_range(vm, instr_addr);
      uint32_t local_offset = 0;
      const bs_decoded_code *decoded = NULL;
      int32_t instr_index = -1;

      if (range == NULL) {
        break;
      }

      local_offset = instr_addr - range->start;
      decoded = &vm->decoded_entries[range->code_entry_index];
      instr_index = bs_decoded_lookup_instruction_index(decoded, local_offset);
      if (instr_index < 0) {
        break;
      }

      vm->decoded_entries[range->code_entry_index].instructions[(size_t)instr_index].variable_index =
          (int32_t)var_idx;
      resolved++;

      if (occ_i < occ_count - 1) {
        const bs_code_entry_data *entry = &vm->game_data->code_entries[range->code_entry_index];
        int32_t raw = 0;
        uint32_t ref_offset = local_offset + 4u;
        uint32_t next_offset = 0;

        if (!bs_read_i32_le_bytes(entry->bytecode, entry->bytecode_length, ref_offset, &raw)) {
          break;
        }

        next_offset = ((uint32_t)raw) & 0x07FFFFFFu;
        instr_addr += next_offset;
      }
    }
  }

  return resolved;
}

static uint32_t bs_resolve_function_chains(bs_vm *vm) {
  uint32_t resolved = 0;
  if (vm == NULL || vm->game_data == NULL) {
    return 0;
  }

  for (size_t func_idx = 0; func_idx < vm->game_data->function_count; func_idx++) {
    const bs_function_data *function = &vm->game_data->functions[func_idx];
    int32_t occ_count = function->occurrence_count;
    uint32_t instr_addr = 0;

    if (occ_count <= 0 || function->first_occurrence_offset < 0) {
      continue;
    }

    instr_addr = (uint32_t)function->first_occurrence_offset;
    for (int32_t occ_i = 0; occ_i < occ_count; occ_i++) {
      const bs_code_range *range = bs_find_code_range(vm, instr_addr);
      uint32_t local_offset = 0;
      const bs_decoded_code *decoded = NULL;
      int32_t instr_index = -1;

      if (range == NULL) {
        break;
      }

      local_offset = instr_addr - range->start;
      decoded = &vm->decoded_entries[range->code_entry_index];
      instr_index = bs_decoded_lookup_instruction_index(decoded, local_offset);
      if (instr_index < 0) {
        break;
      }

      vm->decoded_entries[range->code_entry_index].instructions[(size_t)instr_index].function_index =
          (int32_t)func_idx;
      resolved++;

      if (occ_i < occ_count - 1) {
        const bs_code_entry_data *entry = &vm->game_data->code_entries[range->code_entry_index];
        int32_t raw = 0;
        uint32_t ref_offset = local_offset + 4u;
        uint32_t next_offset = 0;

        if (!bs_read_i32_le_bytes(entry->bytecode, entry->bytecode_length, ref_offset, &raw)) {
          break;
        }

        next_offset = ((uint32_t)raw) & 0x07FFFFFFu;
        instr_addr += next_offset;
      }
    }
  }

  return resolved;
}

static bool bs_vm_execute_code_internal(bs_vm *vm,
                                        size_t code_entry_index,
                                        uint32_t max_instructions,
                                        bool trace,
                                        uint32_t call_depth,
                                        const bs_vm_value *call_args,
                                        size_t call_argc,
                                        bool has_call_args,
                                        bs_vm_execute_result *out_result) {
  bs_vm_execute_result result = {0};
  bs_vm_stack stack = {0};
  bs_vm_locals locals = {0};
  bs_vm_env_stack env_stack = {0};
  const bs_decoded_code *decoded = NULL;
  size_t pc = 0;
  int32_t entry_self_id = -4;
  int32_t entry_other_id = -4;

  result.ok = false;
  result.exit_reason = BS_VM_EXIT_ERROR;
  result.instructions_executed = 0;
  result.return_value_value = bs_vm_value_zero();
  result.return_value = 0.0;

  if (vm == NULL ||
      !vm->initialized ||
      vm->game_data == NULL ||
      code_entry_index >= vm->decoded_entry_count ||
      code_entry_index >= vm->game_data->code_entry_count) {
    if (out_result != NULL) {
      *out_result = result;
    }
    return false;
  }

  decoded = &vm->decoded_entries[code_entry_index];
  entry_self_id = vm->current_self_id;
  entry_other_id = vm->current_other_id;
  if (max_instructions == 0) {
    max_instructions = 200000;
  }
  if (has_call_args && !bs_vm_locals_seed_script_arguments(vm, &locals, call_args, call_argc)) {
    goto execution_error;
  }

  while (pc < decoded->instruction_count && result.instructions_executed < max_instructions) {
    const bs_instruction *instr = &decoded->instructions[pc];
    size_t current_instr_index = pc;
    uint8_t opcode = instr->opcode;

    result.instructions_executed++;
    pc++;

    if (trace) {
      const char *var_name = NULL;
      const char *fn_name = NULL;
      if (instr->variable_index >= 0 &&
          vm->game_data != NULL &&
          (size_t)instr->variable_index < vm->game_data->variable_count) {
        var_name = vm->game_data->variables[(size_t)instr->variable_index].name;
      }
      if (instr->function_index >= 0 &&
          vm->game_data != NULL &&
          (size_t)instr->function_index < vm->game_data->function_count) {
        fn_name = vm->game_data->functions[(size_t)instr->function_index].name;
      }
      printf("    [VM] depth=%u code=%zu pc=%zu op=0x%02X t1=%u t2=%u extra=%d stack=%zu var=%s fn=%s\n",
             (unsigned)call_depth,
             code_entry_index,
             current_instr_index,
             (unsigned)opcode,
             (unsigned)instr->type1,
             (unsigned)instr->type2,
             (int)instr->extra,
             stack.count,
             var_name != NULL ? var_name : "-",
             fn_name != NULL ? fn_name : "-");
    }

    switch (opcode) {
      case BS_OPCODE_PUSH: {
        bs_vm_value value = bs_vm_value_zero();
        switch (instr->type1) {
          case BS_DATA_TYPE_DOUBLE:
            value = bs_vm_value_number(instr->double_value);
            break;
          case BS_DATA_TYPE_FLOAT:
            value = bs_vm_value_number((double)instr->float_value);
            break;
          case BS_DATA_TYPE_INT32:
            value = bs_vm_value_number((double)instr->int_value);
            break;
          case BS_DATA_TYPE_INT64:
            value = bs_vm_value_number((double)instr->long_value);
            break;
          case BS_DATA_TYPE_BOOLEAN:
            value = bs_vm_value_number((instr->int_value != 0) ? 1.0 : 0.0);
            break;
          case BS_DATA_TYPE_STRING:
            if (instr->string_index >= 0 &&
                (size_t)instr->string_index < vm->game_data->string_count) {
              value = bs_vm_value_string(vm->game_data->strings[instr->string_index]);
            } else {
              value = bs_vm_value_string("");
            }
            break;
          case BS_DATA_TYPE_INT16:
            value = bs_vm_value_number((double)instr->int_value);
            break;
          case BS_DATA_TYPE_VARIABLE: {
            if (bs_vm_instruction_is_array(instr)) {
              int32_t array_index = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
              int32_t array_inst_target =
                  (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
              if (bs_vm_variable_is_argument_array(vm, instr->variable_index)) {
                value = bs_vm_locals_argument_get_or_zero(&locals, array_index);
              } else if (array_inst_target == BS_INSTANCE_LOCAL) {
                value = bs_vm_locals_array_get_or_zero(&locals, instr->variable_index, array_index);
              } else if (array_inst_target == BS_INSTANCE_GLOBAL) {
                value = bs_vm_global_array_get_or_zero(vm, instr->variable_index, array_index);
              } else if (bs_vm_builtin_array_get(vm,
                                                 instr->variable_index,
                                                 array_index,
                                                 &value)) {
                /* handled by VM builtin array adapter */
              } else {
                value = bs_vm_instance_get_array_or_zero(vm,
                                                         instr->variable_index,
                                                         array_index,
                                                         bs_vm_resolve_single_instance_target(
                                                             vm,
                                                             array_inst_target));
              }
              break;
            }
            {
              if (bs_vm_variable_is_argument_slot(vm, instr->variable_index)) {
                value = bs_vm_locals_get_or_zero(&locals, instr->variable_index);
                break;
              }
              int32_t effective_inst_type = bs_vm_variable_effective_instance_type(vm, instr);
              int32_t resolved_instance_id = -1;
              bool stacktop_target = bs_vm_instruction_is_stacktop(instr) ||
                                     effective_inst_type == BS_INSTANCE_STACKTOP;
              if (stacktop_target) {
                int32_t stack_target = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
                resolved_instance_id = bs_vm_resolve_single_instance_target(vm, stack_target);
                value = bs_vm_instance_get_for_id_or_zero(vm,
                                                          instr->variable_index,
                                                          resolved_instance_id);
                if (value.type == BS_VM_VALUE_NUMBER &&
                    value.number == 0.0 &&
                    !bs_vm_instance_has_scalar(vm, resolved_instance_id, instr->variable_index) &&
                    bs_vm_instance_has_array(vm, resolved_instance_id, instr->variable_index)) {
                  if (!bs_vm_make_array_ref_value(vm,
                                                  BS_VM_ARRAY_SCOPE_INSTANCE,
                                                  resolved_instance_id,
                                                  instr->variable_index,
                                                  &value)) {
                    goto execution_error;
                  }
                }
              } else if (effective_inst_type == BS_INSTANCE_LOCAL) {
                value = bs_vm_locals_get_or_zero(&locals, instr->variable_index);
                if (value.type == BS_VM_VALUE_NUMBER &&
                    value.number == 0.0 &&
                    !bs_vm_locals_has_scalar(&locals, instr->variable_index) &&
                    bs_vm_locals_has_array(&locals, instr->variable_index)) {
                  if (!bs_vm_make_array_ref_value(vm,
                                                  BS_VM_ARRAY_SCOPE_LOCAL,
                                                  -1,
                                                  instr->variable_index,
                                                  &value)) {
                    goto execution_error;
                  }
                }
              } else if (effective_inst_type == BS_INSTANCE_GLOBAL) {
                value = bs_vm_global_or_builtin_get_or_zero(vm, instr->variable_index);
                if (value.type == BS_VM_VALUE_NUMBER &&
                    value.number == 0.0 &&
                    !bs_vm_global_has_scalar(vm, instr->variable_index) &&
                    bs_vm_global_has_array(vm, instr->variable_index)) {
                  if (!bs_vm_make_array_ref_value(vm,
                                                  BS_VM_ARRAY_SCOPE_GLOBAL,
                                                  -1,
                                                  instr->variable_index,
                                                  &value)) {
                    goto execution_error;
                  }
                }
              } else {
                resolved_instance_id = bs_vm_resolve_single_instance_target(vm, effective_inst_type);
                value = bs_vm_instance_get_for_id_or_zero(vm,
                                                          instr->variable_index,
                                                          resolved_instance_id);
                if (value.type == BS_VM_VALUE_NUMBER &&
                    value.number == 0.0 &&
                    !bs_vm_instance_has_scalar(vm, resolved_instance_id, instr->variable_index) &&
                    bs_vm_instance_has_array(vm, resolved_instance_id, instr->variable_index)) {
                  if (!bs_vm_make_array_ref_value(vm,
                                                  BS_VM_ARRAY_SCOPE_INSTANCE,
                                                  resolved_instance_id,
                                                  instr->variable_index,
                                                  &value)) {
                    goto execution_error;
                  }
                }
              }
            }
            break;
          }
          default:
            value = bs_vm_value_number((double)instr->int_value);
            break;
        }
        if (!bs_vm_stack_push(&stack, value)) {
          goto execution_error;
        }
        break;
      }

      case BS_OPCODE_PUSHI:
        if (!bs_vm_stack_push(&stack, bs_vm_value_number((double)instr->int_value))) {
          goto execution_error;
        }
        break;

      case BS_OPCODE_PUSHLOC:
        if (bs_vm_instruction_is_array(instr)) {
          int32_t array_index = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
          (void)bs_vm_stack_pop_or_zero(&stack);
          if (bs_vm_variable_is_argument_array(vm, instr->variable_index)) {
            if (!bs_vm_stack_push(&stack, bs_vm_locals_argument_get_or_zero(&locals, array_index))) {
              goto execution_error;
            }
            break;
          }
          if (!bs_vm_stack_push(&stack,
                                bs_vm_locals_array_get_or_zero(&locals,
                                                               instr->variable_index,
                                                               array_index))) {
            goto execution_error;
          }
          break;
        }
        {
          bs_vm_value local_value = bs_vm_locals_get_or_zero(&locals, instr->variable_index);
          if (local_value.type == BS_VM_VALUE_NUMBER &&
              local_value.number == 0.0 &&
              !bs_vm_locals_has_scalar(&locals, instr->variable_index) &&
              bs_vm_locals_has_array(&locals, instr->variable_index)) {
            if (!bs_vm_make_array_ref_value(vm,
                                            BS_VM_ARRAY_SCOPE_LOCAL,
                                            -1,
                                            instr->variable_index,
                                            &local_value)) {
              goto execution_error;
            }
          }
          if (!bs_vm_stack_push(&stack, local_value)) {
            goto execution_error;
          }
        }
        break;

      case BS_OPCODE_PUSHGLB:
        if (bs_vm_instruction_is_array(instr)) {
          int32_t array_index = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
          (void)bs_vm_stack_pop_or_zero(&stack);
          if (!bs_vm_stack_push(&stack,
                                bs_vm_global_array_get_or_zero(vm,
                                                               instr->variable_index,
                                                               array_index))) {
            goto execution_error;
          }
          break;
        }
        if (bs_vm_variable_is_global(vm, instr->variable_index)) {
          bs_vm_value global_value = bs_vm_global_or_builtin_get_or_zero(vm, instr->variable_index);
          if (global_value.type == BS_VM_VALUE_NUMBER &&
              global_value.number == 0.0 &&
              !bs_vm_global_has_scalar(vm, instr->variable_index) &&
              bs_vm_global_has_array(vm, instr->variable_index)) {
            if (!bs_vm_make_array_ref_value(vm,
                                            BS_VM_ARRAY_SCOPE_GLOBAL,
                                            -1,
                                            instr->variable_index,
                                            &global_value)) {
              goto execution_error;
            }
          }
          if (!bs_vm_stack_push(&stack, global_value)) {
            goto execution_error;
          }
          break;
        }
        if (!bs_vm_stack_push(&stack, bs_vm_value_zero())) {
          goto execution_error;
        }
        break;

      case BS_OPCODE_PUSHBLTN:
        if (bs_vm_instruction_is_array(instr)) {
          bs_vm_value builtin_array_value = bs_vm_value_zero();
          int32_t array_index = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
          int32_t array_inst_target =
              (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
          if (bs_vm_builtin_array_get(vm,
                                      instr->variable_index,
                                      array_index,
                                      &builtin_array_value)) {
            if (!bs_vm_stack_push(&stack, builtin_array_value)) {
              goto execution_error;
            }
            break;
          }
          if (bs_vm_variable_is_argument_array(vm, instr->variable_index)) {
            if (!bs_vm_stack_push(&stack, bs_vm_locals_argument_get_or_zero(&locals, array_index))) {
              goto execution_error;
            }
            break;
          }
          if (array_inst_target == BS_INSTANCE_LOCAL) {
            if (!bs_vm_stack_push(&stack,
                                  bs_vm_locals_array_get_or_zero(&locals,
                                                                 instr->variable_index,
                                                                 array_index))) {
              goto execution_error;
            }
            break;
          }
          if (array_inst_target == BS_INSTANCE_GLOBAL) {
            if (!bs_vm_stack_push(&stack,
                                  bs_vm_global_array_get_or_zero(vm,
                                                                 instr->variable_index,
                                                                 array_index))) {
              goto execution_error;
            }
            break;
          }
          if (!bs_vm_stack_push(&stack,
                                bs_vm_instance_get_array_or_zero(vm,
                                                                 instr->variable_index,
                                                                 array_index,
                                                                 bs_vm_resolve_single_instance_target(
                                                                     vm,
                                                                     array_inst_target)))) {
            goto execution_error;
          }
          break;
        }
        {
          bs_vm_value read_value = bs_vm_value_zero();
          if (bs_vm_variable_is_argument_slot(vm, instr->variable_index)) {
            if (!bs_vm_stack_push(&stack, bs_vm_locals_get_or_zero(&locals, instr->variable_index))) {
              goto execution_error;
            }
            break;
          }
          int32_t effective_inst_type = bs_vm_variable_effective_instance_type(vm, instr);
          int32_t resolved_instance_id = -1;
          bool stacktop_target = bs_vm_instruction_is_stacktop(instr) ||
                                 effective_inst_type == BS_INSTANCE_STACKTOP;
          if (stacktop_target) {
            int32_t stack_target = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
            resolved_instance_id = bs_vm_resolve_single_instance_target(vm, stack_target);
            read_value = bs_vm_instance_get_for_id_or_zero(vm,
                                                           instr->variable_index,
                                                           resolved_instance_id);
            if (read_value.type == BS_VM_VALUE_NUMBER &&
                read_value.number == 0.0 &&
                !bs_vm_instance_has_scalar(vm, resolved_instance_id, instr->variable_index) &&
                bs_vm_instance_has_array(vm, resolved_instance_id, instr->variable_index)) {
              if (!bs_vm_make_array_ref_value(vm,
                                              BS_VM_ARRAY_SCOPE_INSTANCE,
                                              resolved_instance_id,
                                              instr->variable_index,
                                              &read_value)) {
                goto execution_error;
              }
            }
            if (!bs_vm_stack_push(&stack, read_value)) {
              goto execution_error;
            }
            break;
          }
          if (effective_inst_type == BS_INSTANCE_LOCAL) {
            read_value = bs_vm_locals_get_or_zero(&locals, instr->variable_index);
            if (read_value.type == BS_VM_VALUE_NUMBER &&
                read_value.number == 0.0 &&
                !bs_vm_locals_has_scalar(&locals, instr->variable_index) &&
                bs_vm_locals_has_array(&locals, instr->variable_index)) {
              if (!bs_vm_make_array_ref_value(vm,
                                              BS_VM_ARRAY_SCOPE_LOCAL,
                                              -1,
                                              instr->variable_index,
                                              &read_value)) {
                goto execution_error;
              }
            }
            if (!bs_vm_stack_push(&stack, read_value)) {
              goto execution_error;
            }
            break;
          }
          if (effective_inst_type == BS_INSTANCE_GLOBAL) {
            read_value = bs_vm_global_or_builtin_get_or_zero(vm, instr->variable_index);
            if (read_value.type == BS_VM_VALUE_NUMBER &&
                read_value.number == 0.0 &&
                !bs_vm_global_has_scalar(vm, instr->variable_index) &&
                bs_vm_global_has_array(vm, instr->variable_index)) {
              if (!bs_vm_make_array_ref_value(vm,
                                              BS_VM_ARRAY_SCOPE_GLOBAL,
                                              -1,
                                              instr->variable_index,
                                              &read_value)) {
                goto execution_error;
              }
            }
            if (!bs_vm_stack_push(&stack, read_value)) {
              goto execution_error;
            }
            break;
          }
          resolved_instance_id = bs_vm_resolve_single_instance_target(vm, effective_inst_type);
          read_value = bs_vm_instance_get_for_id_or_zero(vm,
                                                         instr->variable_index,
                                                         resolved_instance_id);
          if (read_value.type == BS_VM_VALUE_NUMBER &&
              read_value.number == 0.0 &&
              !bs_vm_instance_has_scalar(vm, resolved_instance_id, instr->variable_index) &&
              bs_vm_instance_has_array(vm, resolved_instance_id, instr->variable_index)) {
            if (!bs_vm_make_array_ref_value(vm,
                                            BS_VM_ARRAY_SCOPE_INSTANCE,
                                            resolved_instance_id,
                                            instr->variable_index,
                                            &read_value)) {
              goto execution_error;
            }
          }
          if (!bs_vm_stack_push(&stack, read_value)) {
            goto execution_error;
          }
        }
        break;

      case BS_OPCODE_POP: {
        bs_vm_value value = bs_vm_stack_pop_or_zero(&stack);
        if (bs_vm_instruction_is_array(instr)) {
          bool is_compound_array = (instr->type1 != BS_DATA_TYPE_VARIABLE);
          int32_t array_index = 0;
          int32_t array_inst_target = 0;
          if (is_compound_array) {
            array_index = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
            array_inst_target = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
          } else {
            array_index = (int32_t)bs_vm_value_to_number(value);
            array_inst_target = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));
            value = bs_vm_stack_pop_or_zero(&stack);
          }
          if (bs_vm_variable_is_argument_array(vm, instr->variable_index)) {
            break;
          }
          if (array_inst_target == BS_INSTANCE_LOCAL) {
            if (!bs_vm_locals_array_set(vm,
                                        &locals,
                                        instr->variable_index,
                                        array_index,
                                        value)) {
              goto execution_error;
            }
          } else if (array_inst_target == BS_INSTANCE_GLOBAL) {
            if (!bs_vm_global_array_set(vm,
                                        instr->variable_index,
                                        array_index,
                                        value)) {
              goto execution_error;
            }
          } else if (bs_vm_builtin_array_set(vm,
                                             instr->variable_index,
                                             array_index,
                                             value)) {
            /* handled by VM builtin array adapter */
          } else {
            if (!bs_vm_instance_set_array_for_target(vm,
                                                     instr->variable_index,
                                                     array_index,
                                                     array_inst_target,
                                                     value)) {
              goto execution_error;
            }
          }
          break;
        }
        {
          int32_t effective_inst_type = bs_vm_variable_effective_instance_type(vm, instr);
          bool stacktop_target = bs_vm_instruction_is_stacktop(instr) ||
                                 effective_inst_type == BS_INSTANCE_STACKTOP;
          if (stacktop_target) {
            effective_inst_type = (int32_t)bs_vm_value_to_number(value);
            value = bs_vm_stack_pop_or_zero(&stack);
          }

          if (bs_vm_variable_is_argument_slot(vm, instr->variable_index)) {
            if (!bs_vm_locals_set(vm, &locals, instr->variable_index, value)) {
              goto execution_error;
            }
            break;
          }

          {
            bs_vm_array_scope src_scope = BS_VM_ARRAY_SCOPE_INVALID;
            int32_t src_instance_id = -1;
            int32_t src_variable_index = -1;
            if (bs_vm_parse_array_ref_value(value, &src_scope, &src_instance_id, &src_variable_index)) {
              bs_vm_array_scope dst_scope = BS_VM_ARRAY_SCOPE_INVALID;
              int32_t dst_instance_id = -1;
              if (effective_inst_type == BS_INSTANCE_LOCAL) {
                dst_scope = BS_VM_ARRAY_SCOPE_LOCAL;
              } else if (effective_inst_type == BS_INSTANCE_GLOBAL) {
                dst_scope = BS_VM_ARRAY_SCOPE_GLOBAL;
              } else {
                dst_scope = BS_VM_ARRAY_SCOPE_INSTANCE;
                dst_instance_id = bs_vm_resolve_single_instance_target(vm, effective_inst_type);
              }
              if (dst_scope == BS_VM_ARRAY_SCOPE_INSTANCE && dst_instance_id < 0) {
                break;
              }
              if (src_scope == BS_VM_ARRAY_SCOPE_INSTANCE && src_instance_id < 0) {
                break;
              }
              if (!bs_vm_copy_array_variable(vm,
                                             &locals,
                                             src_scope,
                                             src_instance_id,
                                             src_variable_index,
                                             dst_scope,
                                             dst_instance_id,
                                             instr->variable_index)) {
                goto execution_error;
              }
              break;
            }
          }

          if (effective_inst_type == BS_INSTANCE_LOCAL) {
            if (!bs_vm_locals_set(vm, &locals, instr->variable_index, value)) {
              goto execution_error;
            }
          } else if (effective_inst_type == BS_INSTANCE_GLOBAL) {
            if (!bs_vm_global_set(vm, instr->variable_index, value)) {
              goto execution_error;
            }
          } else if (!bs_vm_instance_set_for_target(vm,
                                                    instr->variable_index,
                                                    effective_inst_type,
                                                    value)) {
            goto execution_error;
          }
        }
        break;
      }

      case BS_OPCODE_POPZ:
        (void)bs_vm_stack_pop_or_zero(&stack);
        break;

      case BS_OPCODE_DUP: {
        size_t dup_count = 1u;
        if (instr->extra > 0) {
          dup_count = (size_t)instr->extra + 1u;
        }

        if (stack.count >= dup_count && dup_count > 1u) {
          bs_vm_value *items = (bs_vm_value *)malloc(dup_count * sizeof(bs_vm_value));
          if (items == NULL) {
            goto execution_error;
          }
          for (size_t i = 0; i < dup_count; i++) {
            items[dup_count - 1u - i] = bs_vm_stack_pop_or_zero(&stack);
          }
          for (size_t i = 0; i < dup_count; i++) {
            if (!bs_vm_stack_push(&stack, items[i])) {
              free(items);
              goto execution_error;
            }
          }
          for (size_t i = 0; i < dup_count; i++) {
            if (!bs_vm_stack_push(&stack, items[i])) {
              free(items);
              goto execution_error;
            }
          }
          free(items);
        } else {
          bs_vm_value top = bs_vm_stack_peek_or_zero(&stack);
          if (!bs_vm_stack_push(&stack, top)) {
            goto execution_error;
          }
        }
        break;
      }

      case BS_OPCODE_CONV:
        break;

      case BS_OPCODE_NEG: {
        bs_vm_value value = bs_vm_stack_pop_or_zero(&stack);
        if (!bs_vm_stack_push(&stack, bs_vm_value_number(-bs_vm_value_to_number(value)))) {
          goto execution_error;
        }
        break;
      }

      case BS_OPCODE_NOT: {
        bs_vm_value value = bs_vm_stack_pop_or_zero(&stack);
        if (!bs_vm_stack_push(&stack, bs_vm_value_number(bs_vm_value_to_bool(value) ? 0.0 : 1.0))) {
          goto execution_error;
        }
        break;
      }

      case BS_OPCODE_MUL:
      case BS_OPCODE_DIV:
      case BS_OPCODE_ADD:
      case BS_OPCODE_SUB:
        if (!bs_vm_binary_real_op(vm, &stack, opcode)) {
          goto execution_error;
        }
        break;

      case BS_OPCODE_REM:
      case BS_OPCODE_MOD:
      case BS_OPCODE_AND:
      case BS_OPCODE_OR:
      case BS_OPCODE_XOR:
      case BS_OPCODE_SHL:
      case BS_OPCODE_SHR:
        if (!bs_vm_binary_int_op(&stack, opcode)) {
          goto execution_error;
        }
        break;

      case BS_OPCODE_CMP: {
        bs_vm_value rhs = bs_vm_stack_pop_or_zero(&stack);
        bs_vm_value lhs = bs_vm_stack_pop_or_zero(&stack);
        int cmp = bs_vm_compare_values(lhs, rhs);
        uint8_t comparison_type = (uint8_t)((instr->raw_operand >> 8) & 0xFFu);
        if (!bs_vm_stack_push(&stack,
                              bs_vm_value_number(bs_vm_compare_bool(cmp, comparison_type) ? 1.0 : 0.0))) {
          goto execution_error;
        }
        break;
      }

      case BS_OPCODE_B: {
        size_t target = 0;
        int32_t branch_offset = bs_vm_branch_offset(instr->raw_operand);
        if (!bs_vm_find_branch_target(decoded, current_instr_index, branch_offset, &target)) {
          if (trace) {
            uint32_t cur_off = decoded->instruction_offsets[current_instr_index];
            int64_t tgt_off = (int64_t)cur_off + ((int64_t)branch_offset * 4ll);
            printf("    [VM BRANCH MISS] code=%zu pc=%zu op=B cur_off=%u branch=%d target_off=%lld\n",
                   code_entry_index,
                   current_instr_index,
                   (unsigned)cur_off,
                   (int)branch_offset,
                   (long long)tgt_off);
          }
          result.exit_reason = BS_VM_EXIT_OUT_OF_RANGE;
          goto execution_done;
        }
        pc = target;
        break;
      }

      case BS_OPCODE_BT:
      case BS_OPCODE_BF: {
        bs_vm_value condition = bs_vm_stack_pop_or_zero(&stack);
        bool cond = bs_vm_value_to_bool(condition);
        bool should_branch = ((opcode == BS_OPCODE_BT && cond) || (opcode == BS_OPCODE_BF && !cond));
        if (should_branch) {
          size_t target = 0;
          int32_t branch_offset = bs_vm_branch_offset(instr->raw_operand);
          if (!bs_vm_find_branch_target(decoded, current_instr_index, branch_offset, &target)) {
            if (trace) {
              uint32_t cur_off = decoded->instruction_offsets[current_instr_index];
              int64_t tgt_off = (int64_t)cur_off + ((int64_t)branch_offset * 4ll);
              printf("    [VM BRANCH MISS] code=%zu pc=%zu op=%s cur_off=%u branch=%d target_off=%lld\n",
                     code_entry_index,
                     current_instr_index,
                     (opcode == BS_OPCODE_BT) ? "BT" : "BF",
                     (unsigned)cur_off,
                     (int)branch_offset,
                     (long long)tgt_off);
            }
            result.exit_reason = BS_VM_EXIT_OUT_OF_RANGE;
            goto execution_done;
          }
          pc = target;
        }
        break;
      }

      case BS_OPCODE_PUSHENV: {
        int32_t *instance_ids = NULL;
        size_t instance_count = 0;
        size_t first_index = 0;
        int32_t first_instance_id = -4;
        bs_vm_env_iteration frame = {0};
        int32_t branch_offset = bs_vm_branch_offset(instr->raw_operand);
        int32_t target_id = (int32_t)bs_vm_value_to_number(bs_vm_stack_pop_or_zero(&stack));

        if (!bs_vm_collect_target_instance_ids(vm, target_id, &instance_ids, &instance_count)) {
          goto execution_error;
        }

        if (!bs_vm_find_next_alive_instance_id(vm,
                                               instance_ids,
                                               instance_count,
                                               0u,
                                               &first_index,
                                               &first_instance_id)) {
          size_t target = 0;
          free(instance_ids);
          if (!bs_vm_find_branch_target(decoded, current_instr_index, branch_offset, &target)) {
            if (trace) {
              uint32_t cur_off = decoded->instruction_offsets[current_instr_index];
              int64_t tgt_off = (int64_t)cur_off + ((int64_t)branch_offset * 4ll);
              printf("    [VM BRANCH MISS] code=%zu pc=%zu op=PUSHENV cur_off=%u branch=%d target_off=%lld\n",
                     code_entry_index,
                     current_instr_index,
                     (unsigned)cur_off,
                     (int)branch_offset,
                     (long long)tgt_off);
            }
            result.exit_reason = BS_VM_EXIT_OUT_OF_RANGE;
            goto execution_done;
          }
          pc = target;
          break;
        }

        frame.instance_ids = instance_ids;
        frame.instance_count = instance_count;
        frame.current_index = first_index;
        frame.prev_self_id = vm->current_self_id;
        frame.prev_other_id = vm->current_other_id;
        if (!bs_vm_env_stack_push(&env_stack, frame)) {
          free(instance_ids);
          goto execution_error;
        }

        vm->current_other_id = vm->current_self_id;
        vm->current_self_id = first_instance_id;
        break;
      }

      case BS_OPCODE_POPENV: {
        bs_vm_env_iteration *iter = bs_vm_env_stack_last(&env_stack);
        if (iter != NULL) {
          size_t next_index = iter->current_index + 1u;
          size_t found_index = 0;
          int32_t found_instance_id = -4;
          if (bs_vm_find_next_alive_instance_id(vm,
                                                iter->instance_ids,
                                                iter->instance_count,
                                                next_index,
                                                &found_index,
                                                &found_instance_id)) {
            size_t target = 0;
            int32_t branch_offset = bs_vm_branch_offset(instr->raw_operand);
            iter->current_index = found_index;
            vm->current_self_id = found_instance_id;
            if (!bs_vm_find_branch_target(decoded, current_instr_index, branch_offset, &target)) {
              if (trace) {
                uint32_t cur_off = decoded->instruction_offsets[current_instr_index];
                int64_t tgt_off = (int64_t)cur_off + ((int64_t)branch_offset * 4ll);
                printf("    [VM BRANCH MISS] code=%zu pc=%zu op=POPENV cur_off=%u branch=%d target_off=%lld\n",
                       code_entry_index,
                       current_instr_index,
                       (unsigned)cur_off,
                       (int)branch_offset,
                       (long long)tgt_off);
              }
              result.exit_reason = BS_VM_EXIT_OUT_OF_RANGE;
              goto execution_done;
            }
            pc = target;
          } else {
            vm->current_self_id = iter->prev_self_id;
            vm->current_other_id = iter->prev_other_id;
            bs_vm_env_stack_pop(&env_stack);
          }
        }
        break;
      }

      case BS_OPCODE_CALL: {
        uint16_t argc = (uint16_t)instr->extra;
        bs_vm_value call_result = bs_vm_value_zero();
        bs_vm_value stored_call_result = bs_vm_value_zero();
        bs_vm_value *args = NULL;

        if (argc > 0) {
          args = (bs_vm_value *)calloc(argc, sizeof(bs_vm_value));
          if (args == NULL) {
            goto execution_error;
          }
          for (uint16_t i = 0; i < argc; i++) {
            args[i] = bs_vm_stack_pop_or_zero(&stack);
          }
        }

        if (instr->function_index >= 0 && (size_t)instr->function_index < vm->game_data->function_count) {
          const char *function_name = vm->game_data->functions[instr->function_index].name;
          int32_t script_code_id = bs_vm_find_script_code_id(vm->game_data, function_name);
          bs_vm_builtin_callback builtin_cb = bs_vm_find_builtin(vm, function_name);
          if (trace) {
            printf("      CALL %s argc=%u\n", function_name, (unsigned)argc);
          }
          if (script_code_id >= 0 &&
              (size_t)script_code_id < vm->game_data->code_entry_count &&
              call_depth < BS_VM_MAX_CALL_DEPTH) {
            bs_vm_execute_result nested = {0};
            uint32_t nested_max_instructions = max_instructions;
            if (nested_max_instructions > 60000u) {
              nested_max_instructions = 60000u;
            }
            if (bs_vm_execute_code_internal(vm,
                                            (size_t)script_code_id,
                                            nested_max_instructions,
                                            trace,
                                            call_depth + 1u,
                                            args,
                                            (size_t)argc,
                                            true,
                                            &nested)) {
              call_result = nested.return_value_value;
            }
          } else if (builtin_cb != NULL) {
            call_result = builtin_cb(vm, args, (size_t)argc);
          } else if (instr->function_index >= 0 &&
                     (size_t)instr->function_index < vm->unknown_function_logged_count) {
            size_t function_index = (size_t)instr->function_index;
            if (!vm->unknown_function_logged[function_index]) {
              vm->unknown_function_logged[function_index] = true;
              printf("  VM NOTE: unknown function '%s' argc=%u\n",
                     function_name != NULL ? function_name : "<unnamed>",
                     (unsigned)argc);
            }
          }
        }

        free(args);
        if (!bs_vm_make_storable_value(vm, call_result, &stored_call_result)) {
          goto execution_error;
        }
        if (!bs_vm_stack_push(&stack, stored_call_result)) {
          goto execution_error;
        }
        break;
      }

      case BS_OPCODE_RET:
        result.return_value_value = bs_vm_stack_pop_or_zero(&stack);
        result.return_value = bs_vm_value_to_number(result.return_value_value);
        result.exit_reason = BS_VM_EXIT_RET;
        goto execution_done;

      case BS_OPCODE_EXIT:
        result.exit_reason = BS_VM_EXIT_EXIT;
        goto execution_done;

      default:
        break;
    }
  }

  if (result.instructions_executed >= max_instructions) {
    result.exit_reason = BS_VM_EXIT_MAX_INSTRUCTIONS;
  } else {
    result.exit_reason = BS_VM_EXIT_OUT_OF_RANGE;
  }
  goto execution_done;

execution_error:
  vm->current_self_id = entry_self_id;
  vm->current_other_id = entry_other_id;
  result.ok = false;
  result.exit_reason = BS_VM_EXIT_ERROR;
  bs_vm_env_stack_dispose(&env_stack);
  bs_vm_locals_dispose(&locals);
  bs_vm_stack_dispose(&stack);
  if (out_result != NULL) {
    *out_result = result;
  }
  return false;

execution_done:
  vm->current_self_id = entry_self_id;
  vm->current_other_id = entry_other_id;
  result.ok = true;
  bs_vm_env_stack_dispose(&env_stack);
  bs_vm_locals_dispose(&locals);
  bs_vm_stack_dispose(&stack);
  if (out_result != NULL) {
    *out_result = result;
  }
  return true;
}

bool bs_vm_execute_code(bs_vm *vm,
                        size_t code_entry_index,
                        uint32_t max_instructions,
                        bool trace,
                        bs_vm_execute_result *out_result) {
  return bs_vm_execute_code_internal(vm,
                                     code_entry_index,
                                     max_instructions,
                                     trace,
                                     0u,
                                     NULL,
                                     0u,
                                     false,
                                     out_result);
}

bool bs_vm_execute_code_with_args(bs_vm *vm,
                                  size_t code_entry_index,
                                  const bs_vm_value *args,
                                  size_t argc,
                                  uint32_t max_instructions,
                                  bool trace,
                                  bs_vm_execute_result *out_result) {
  return bs_vm_execute_code_internal(vm,
                                     code_entry_index,
                                     max_instructions,
                                     trace,
                                     0u,
                                     args,
                                     argc,
                                     true,
                                     out_result);
}

void bs_vm_init(bs_vm *vm, const bs_game_data *game_data) {
  uint32_t resolved_variables = 0;
  uint32_t resolved_functions = 0;
  const char *debug_code_env = NULL;

  if (vm == NULL) {
    return;
  }

  vm->game_data = game_data;
  vm->runner = NULL;
  vm->decoded_entries = NULL;
  vm->decoded_entry_count = 0;
  vm->code_ranges = NULL;
  vm->code_range_count = 0;
  vm->global_variable_indices = NULL;
  vm->global_variable_values = NULL;
  vm->global_variable_count = 0;
  vm->global_variable_capacity = 0;
  vm->global_array_variable_indices = NULL;
  vm->global_array_element_indices = NULL;
  vm->global_array_values = NULL;
  vm->global_array_count = 0;
  vm->global_array_capacity = 0;
  vm->instance_variable_instance_ids = NULL;
  vm->instance_variable_indices = NULL;
  vm->instance_variable_values = NULL;
  vm->instance_variable_count = 0;
  vm->instance_variable_capacity = 0;
  vm->instance_array_instance_ids = NULL;
  vm->instance_array_variable_indices = NULL;
  vm->instance_array_element_indices = NULL;
  vm->instance_array_values = NULL;
  vm->instance_array_count = 0;
  vm->instance_array_capacity = 0;
  vm->owned_strings = NULL;
  vm->owned_string_count = 0;
  vm->owned_string_capacity = 0;
  vm->argument_array_variable_index = -1;
  vm->argument_count_variable_index = -1;
  for (int i = 0; i < 16; i++) {
    vm->argument_slot_variable_indices[i] = -1;
  }
  vm->builtin_names = NULL;
  vm->builtin_callbacks = NULL;
  vm->builtin_count = 0;
  vm->builtin_capacity = 0;
  vm->current_self_id = -4;
  vm->current_other_id = -4;
  vm->unknown_function_logged = NULL;
  vm->unknown_function_logged_count = 0;
  vm->initialized = false;

  if (game_data == NULL) {
    return;
  }
  bs_vm_cache_argument_variable_indices(vm);
  if (getenv("BS_TRACE_SCRIPT_EXECUTE") != NULL) {
    printf("VM arg vars: argument=%d argument_count=%d argument0=%d argument1=%d\n",
           vm->argument_array_variable_index,
           vm->argument_count_variable_index,
           vm->argument_slot_variable_indices[0],
           vm->argument_slot_variable_indices[1]);
  }
  debug_code_env = getenv("BS_DEBUG_CODE");

  if (game_data->function_count > 0) {
    vm->unknown_function_logged = (bool *)calloc(game_data->function_count, sizeof(bool));
    if (vm->unknown_function_logged == NULL) {
      bs_vm_dispose(vm);
      return;
    }
  }
  vm->unknown_function_logged_count = game_data->function_count;

  if (game_data->code_entry_count > 0) {
    vm->decoded_entries =
        (bs_decoded_code *)calloc(game_data->code_entry_count, sizeof(bs_decoded_code));
    if (vm->decoded_entries == NULL) {
      return;
    }
  }
  vm->decoded_entry_count = game_data->code_entry_count;

  for (size_t i = 0; i < vm->decoded_entry_count; i++) {
    if (!bs_decode_bytecode(&game_data->code_entries[i], &vm->decoded_entries[i])) {
      fprintf(stderr, "Failed to decode bytecode for CODE[%zu] '%s'\n",
              i,
              game_data->code_entries[i].name != NULL ? game_data->code_entries[i].name : "<unnamed>");
      bs_vm_dispose(vm);
      return;
    }
  }

  if (!bs_build_code_ranges(vm)) {
    fprintf(stderr, "Failed to build CODE ranges for VM\n");
    bs_vm_dispose(vm);
    return;
  }

  resolved_variables = bs_resolve_variable_chains(vm);
  resolved_functions = bs_resolve_function_chains(vm);

  if (debug_code_env != NULL && strcmp(debug_code_env, "1") == 0) {
    int debug_codes[] = {419, 420, 522, 524, 5507};
    for (size_t i = 0; i < sizeof(debug_codes) / sizeof(debug_codes[0]); i++) {
      int code_id = debug_codes[i];
      if (code_id >= 0 && (size_t)code_id < vm->game_data->code_entry_count) {
        const bs_code_entry_data *entry = &vm->game_data->code_entries[(size_t)code_id];
        const bs_decoded_code *decoded = &vm->decoded_entries[(size_t)code_id];
        printf("  [DEBUG CODE] id=%d name=%s bytecode_len=%u instr_count=%zu\n",
               code_id,
               entry->name != NULL ? entry->name : "<unnamed>",
               (unsigned)entry->bytecode_length,
               decoded->instruction_count);
      }
    }
  }

  vm->initialized = true;
  printf("VM initialized: %zu code entries decoded\n", vm->decoded_entry_count);
  printf("  Resolved %u variable references\n", resolved_variables);
  printf("  Resolved %u function references\n", resolved_functions);
}

void bs_vm_dispose(bs_vm *vm) {
  if (vm == NULL) {
    return;
  }

  if (vm->decoded_entries != NULL) {
    for (size_t i = 0; i < vm->decoded_entry_count; i++) {
      bs_decoded_code_free(&vm->decoded_entries[i]);
    }
    free(vm->decoded_entries);
  }
  vm->decoded_entries = NULL;
  vm->decoded_entry_count = 0;

  free(vm->code_ranges);
  vm->code_ranges = NULL;
  vm->code_range_count = 0;

  free(vm->global_variable_indices);
  free(vm->global_variable_values);
  free(vm->global_array_variable_indices);
  free(vm->global_array_element_indices);
  free(vm->global_array_values);
  vm->global_variable_indices = NULL;
  vm->global_variable_values = NULL;
  vm->global_variable_count = 0;
  vm->global_variable_capacity = 0;
  vm->global_array_variable_indices = NULL;
  vm->global_array_element_indices = NULL;
  vm->global_array_values = NULL;
  vm->global_array_count = 0;
  vm->global_array_capacity = 0;

  free(vm->instance_variable_instance_ids);
  free(vm->instance_variable_indices);
  free(vm->instance_variable_values);
  vm->instance_variable_instance_ids = NULL;
  vm->instance_variable_indices = NULL;
  vm->instance_variable_values = NULL;
  vm->instance_variable_count = 0;
  vm->instance_variable_capacity = 0;

  free(vm->instance_array_instance_ids);
  free(vm->instance_array_variable_indices);
  free(vm->instance_array_element_indices);
  free(vm->instance_array_values);
  vm->instance_array_instance_ids = NULL;
  vm->instance_array_variable_indices = NULL;
  vm->instance_array_element_indices = NULL;
  vm->instance_array_values = NULL;
  vm->instance_array_count = 0;
  vm->instance_array_capacity = 0;

  if (vm->owned_strings != NULL) {
    for (size_t i = 0; i < vm->owned_string_count; i++) {
      free(vm->owned_strings[i]);
    }
  }
  free(vm->owned_strings);
  vm->owned_strings = NULL;
  vm->owned_string_count = 0;
  vm->owned_string_capacity = 0;

  vm->argument_array_variable_index = -1;
  vm->argument_count_variable_index = -1;
  for (int i = 0; i < 16; i++) {
    vm->argument_slot_variable_indices[i] = -1;
  }

  if (vm->builtin_names != NULL) {
    for (size_t i = 0; i < vm->builtin_count; i++) {
      free(vm->builtin_names[i]);
    }
  }
  free(vm->builtin_names);
  free(vm->builtin_callbacks);
  vm->builtin_names = NULL;
  vm->builtin_callbacks = NULL;
  vm->builtin_count = 0;
  vm->builtin_capacity = 0;

  vm->initialized = false;
  vm->game_data = NULL;
  vm->runner = NULL;
  vm->current_self_id = -4;
  vm->current_other_id = -4;
  free(vm->unknown_function_logged);
  vm->unknown_function_logged = NULL;
  vm->unknown_function_logged_count = 0;
}
