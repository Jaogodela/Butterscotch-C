#ifndef BS_VM_VM_H
#define BS_VM_VM_H

#include "bs/common.h"
#include "bs/data/form_reader.h"

struct bs_game_runner;
struct bs_vm;

typedef enum bs_vm_value_type {
  BS_VM_VALUE_NUMBER = 0,
  BS_VM_VALUE_STRING = 1
} bs_vm_value_type;

typedef struct bs_vm_value {
  bs_vm_value_type type;
  double number;
  const char *string;
} bs_vm_value;

static inline bs_vm_value bs_vm_make_number(double number) {
  bs_vm_value value;
  value.type = BS_VM_VALUE_NUMBER;
  value.number = number;
  value.string = NULL;
  return value;
}

static inline bs_vm_value bs_vm_make_string(const char *string) {
  bs_vm_value value;
  value.type = BS_VM_VALUE_STRING;
  value.number = 0.0;
  value.string = string;
  return value;
}

typedef bs_vm_value (*bs_vm_builtin_callback)(struct bs_vm *vm, const bs_vm_value *args, size_t argc);

typedef enum bs_opcode {
  BS_OPCODE_CONV = 0x07,
  BS_OPCODE_MUL = 0x08,
  BS_OPCODE_DIV = 0x09,
  BS_OPCODE_REM = 0x0A,
  BS_OPCODE_MOD = 0x0B,
  BS_OPCODE_ADD = 0x0C,
  BS_OPCODE_SUB = 0x0D,
  BS_OPCODE_AND = 0x0E,
  BS_OPCODE_OR = 0x0F,
  BS_OPCODE_XOR = 0x10,
  BS_OPCODE_NEG = 0x11,
  BS_OPCODE_NOT = 0x12,
  BS_OPCODE_SHL = 0x13,
  BS_OPCODE_SHR = 0x14,
  BS_OPCODE_CMP = 0x15,
  BS_OPCODE_POP = 0x45,
  BS_OPCODE_PUSHI = 0x84,
  BS_OPCODE_DUP = 0x86,
  BS_OPCODE_RET = 0x9C,
  BS_OPCODE_EXIT = 0x9D,
  BS_OPCODE_POPZ = 0x9E,
  BS_OPCODE_B = 0xB6,
  BS_OPCODE_BT = 0xB7,
  BS_OPCODE_BF = 0xB8,
  BS_OPCODE_PUSHENV = 0xBA,
  BS_OPCODE_POPENV = 0xBB,
  BS_OPCODE_PUSH = 0xC0,
  BS_OPCODE_PUSHLOC = 0xC1,
  BS_OPCODE_PUSHGLB = 0xC2,
  BS_OPCODE_PUSHBLTN = 0xC3,
  BS_OPCODE_CALL = 0xD9,
  BS_OPCODE_BREAK = 0xFF
} bs_opcode;

typedef enum bs_data_type {
  BS_DATA_TYPE_DOUBLE = 0x0,
  BS_DATA_TYPE_FLOAT = 0x1,
  BS_DATA_TYPE_INT32 = 0x2,
  BS_DATA_TYPE_INT64 = 0x3,
  BS_DATA_TYPE_BOOLEAN = 0x4,
  BS_DATA_TYPE_VARIABLE = 0x5,
  BS_DATA_TYPE_STRING = 0x6,
  BS_DATA_TYPE_INT16 = 0x0F
} bs_data_type;

typedef enum bs_comparison_type {
  BS_COMPARISON_LT = 1,
  BS_COMPARISON_LTE = 2,
  BS_COMPARISON_EQ = 3,
  BS_COMPARISON_NEQ = 4,
  BS_COMPARISON_GTE = 5,
  BS_COMPARISON_GT = 6
} bs_comparison_type;

typedef enum bs_instance_type {
  BS_INSTANCE_SELF = -1,
  BS_INSTANCE_OTHER = -2,
  BS_INSTANCE_ALL = -3,
  BS_INSTANCE_NOONE = -4,
  BS_INSTANCE_GLOBAL = -5,
  BS_INSTANCE_BUILTIN = -6,
  BS_INSTANCE_LOCAL = -7,
  BS_INSTANCE_STACKTOP = -9
} bs_instance_type;

typedef enum bs_vm_exit_reason {
  BS_VM_EXIT_NONE = 0,
  BS_VM_EXIT_RET = 1,
  BS_VM_EXIT_EXIT = 2,
  BS_VM_EXIT_OUT_OF_RANGE = 3,
  BS_VM_EXIT_MAX_INSTRUCTIONS = 4,
  BS_VM_EXIT_ERROR = 5
} bs_vm_exit_reason;

typedef struct bs_instruction {
  uint8_t opcode;
  uint8_t type1;
  uint8_t type2;
  int16_t extra;
  uint32_t raw_operand;

  int32_t variable_index;
  int32_t variable_type;
  int32_t function_index;

  int32_t int_value;
  int64_t long_value;
  double double_value;
  float float_value;
  int32_t string_index;
} bs_instruction;

typedef struct bs_decoded_code {
  bs_instruction *instructions;
  uint32_t *instruction_offsets;
  size_t instruction_count;
} bs_decoded_code;

typedef struct bs_code_range {
  uint32_t start;
  uint32_t end;
  size_t code_entry_index;
} bs_code_range;

typedef struct bs_vm_execute_result {
  bool ok;
  bs_vm_exit_reason exit_reason;
  uint32_t instructions_executed;
  bs_vm_value return_value_value;
  double return_value;
} bs_vm_execute_result;

typedef struct bs_vm {
  const bs_game_data *game_data;
  struct bs_game_runner *runner;
  bs_decoded_code *decoded_entries;
  size_t decoded_entry_count;
  bs_code_range *code_ranges;
  size_t code_range_count;

  int32_t *global_variable_indices;
  bs_vm_value *global_variable_values;
  size_t global_variable_count;
  size_t global_variable_capacity;
  int32_t *global_array_variable_indices;
  int32_t *global_array_element_indices;
  bs_vm_value *global_array_values;
  size_t global_array_count;
  size_t global_array_capacity;

  int32_t *instance_variable_instance_ids;
  int32_t *instance_variable_indices;
  bs_vm_value *instance_variable_values;
  size_t instance_variable_count;
  size_t instance_variable_capacity;

  int32_t *instance_array_instance_ids;
  int32_t *instance_array_variable_indices;
  int32_t *instance_array_element_indices;
  bs_vm_value *instance_array_values;
  size_t instance_array_count;
  size_t instance_array_capacity;

  char **owned_strings;
  size_t owned_string_count;
  size_t owned_string_capacity;

  int32_t argument_array_variable_index;
  int32_t argument_count_variable_index;
  int32_t argument_slot_variable_indices[16];

  char **builtin_names;
  bs_vm_builtin_callback *builtin_callbacks;
  size_t builtin_count;
  size_t builtin_capacity;

  int32_t current_self_id;
  int32_t current_other_id;

  bool *unknown_function_logged;
  size_t unknown_function_logged_count;

  bool initialized;
} bs_vm;

void bs_vm_init(bs_vm *vm, const bs_game_data *game_data);
void bs_vm_dispose(bs_vm *vm);
bool bs_vm_register_builtin(bs_vm *vm, const char *name, bs_vm_builtin_callback callback);
bool bs_vm_execute_code(bs_vm *vm,
                        size_t code_entry_index,
                        uint32_t max_instructions,
                        bool trace,
                        bs_vm_execute_result *out_result);
bool bs_vm_execute_code_with_args(bs_vm *vm,
                                  size_t code_entry_index,
                                  const bs_vm_value *args,
                                  size_t argc,
                                  uint32_t max_instructions,
                                  bool trace,
                                  bs_vm_execute_result *out_result);

#endif
