idf_component_register(
  SRCS "src/wren_compiler.c"
       "src/wren_core.c"
       "src/wren_debug.c"
       "src/wren_primitive.c"
       "src/wren_utils.c"
       "src/wren_value.c"
       "src/wren_vm.c"
  INCLUDE_DIRS "src"
)

# Stessi flag di lib/wren/library.json: niente moduli opzionali meta/random.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WREN_OPT_META=0 WREN_OPT_RANDOM=0)

# Wren upstream usa "%.*s" con un int32_t (long su xtensa) e, con i moduli
# opzionali spenti, lascia qualche variabile inutilizzata. E' una libreria
# third-party: questi warning non devono fermare la build IDF.
target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wno-error=format
  -Wno-error=uninitialized
  -Wno-error=unused-variable
)
