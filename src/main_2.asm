; Minimal pomodoro — x86-64 Linux, NASM, raw syscalls, no libc.
;
; Loops forever: 4 × (25 min focus + 5 min short break), then a 15 min long
; break, then repeats. The only output is a terminal bell (BEL, 0x07) written
; to stdout at the end of each session. No UI, no input, no configuration.
; Ctrl+C to quit.

bits 64

%define SYS_write       1
%define SYS_nanosleep   35

%define FOCUS_MIN       25
%define SHORT_MIN       5
%define LONG_MIN        15
%define ROUNDS          4

section .rodata
    bell: db 0x07

section .text
global _start

; sleep for rdi minutes
sleep_minutes:
    imul    rdi, 60
    sub     rsp, 16
    mov     [rsp], rdi              ; tv_sec
    mov     qword [rsp + 8], 0      ; tv_nsec
    mov     rdi, rsp
    xor     rsi, rsi
    mov     rax, SYS_nanosleep
    syscall
    add     rsp, 16
    ret

; write BEL to stdout
ring:
    mov     rax, SYS_write
    mov     rdi, 1
    lea     rsi, [rel bell]
    mov     rdx, 1
    syscall
    ret

_start:
.cycle:
    xor     ebx, ebx                ; focus rounds completed in this cycle
.focus:
    mov     rdi, FOCUS_MIN
    call    sleep_minutes
    call    ring
    inc     ebx
    cmp     ebx, ROUNDS
    je      .long_break
    mov     rdi, SHORT_MIN
    call    sleep_minutes
    call    ring
    jmp     .focus
.long_break:
    mov     rdi, LONG_MIN
    call    sleep_minutes
    call    ring
    jmp     .cycle
