/* PCSX2 PS4 port (AI-assisted): the eboot's code range, which PS5SX2's linker script provides
 * (ps5/coreorbis/orbis-shims/ehframe.ld) and orbis_eeprof.cpp reads. */
PROVIDE(__orbis_text_start = ADDR(.text));
PROVIDE(__orbis_text_end = ADDR(.text) + SIZEOF(.text));
