# Compile options shared by the freestanding AArch64 images and models: no
# hosted library or builtins, no stack protector or unwind tables, strict
# alignment for Device memory, and one section per function and object for
# --gc-sections. Targets append their own options, such as -Werror, after
# these.
set(SUNSTONE_KERNEL_FLAGS -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra)
