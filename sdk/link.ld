/* Added code lives ONLY in the free tail of flash sector 8.
   The v118 firmware ends at 0x0809C500; sector 8 ends at 0x080A0000. The pedal's USB
   bootloader erases and programs that sector (which is why the USER engine, living here,
   flashes fine). Writing past 0x080A0000 does NOT work: the bootloader does not erase
   sectors 9+, and the upload aborts partway. */
MEMORY
{
  FLASH_EXT (rx) : ORIGIN = 0x0809C800, LENGTH = 0x3800   /* ~14 KB up to 0x080A0000 */
}

ENTRY(user_hook)

SECTIONS
{
  .text :
  {
    KEEP(*(.hook))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH_EXT

  /* No free fixed RAM on the pedal: no writable globals allowed. All state lives in
     CCM RAM (0x10000000), accessed through a pointer and initialised in activate(). */
  .data : { *(.data*) } > FLASH_EXT
  .bss  : { *(.bss*) *(COMMON) } > FLASH_EXT
  ASSERT(SIZEOF(.data) == 0, "No initialised globals (.data) allowed; use CCM RAM")
  ASSERT(SIZEOF(.bss)  == 0, "No globals (.bss) allowed; use CCM RAM")

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.ARM.attributes) *(.comment) *(.eh_frame*) }
}
