# uint32_t call_raw(void *fn, uint32_t ecx, const uint32_t *args, int n)   (cdecl)
# Calls fn with ECX = ecx and n dword args pushed right-to-left. Works for __thiscall, __stdcall and __cdecl
# callees because ESP is restored from EBP afterwards.
    .text
    .globl _call_raw
_call_raw:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl  20(%ebp), %edx
    movl  16(%ebp), %esi
1:  testl %edx, %edx
    jz    2f
    decl  %edx
    pushl (%esi,%edx,4)
    jmp   1b
2:  movl  12(%ebp), %ecx
    movl  8(%ebp), %eax
    call  *%eax
    leal  -12(%ebp), %esp
    popl  %ebx
    popl  %edi
    popl  %esi
    popl  %ebp
    ret
