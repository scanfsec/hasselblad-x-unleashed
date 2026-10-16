/* ARM64 module loader requires these expandable trampoline sections. */
SECTIONS
{
    .plt (NOLOAD) : { BYTE(0) }
    .init.plt (NOLOAD) : { BYTE(0) }
    .text.ftrace_trampoline (NOLOAD) : { BYTE(0) }
}
