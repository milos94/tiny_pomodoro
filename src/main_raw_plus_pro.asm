; main_raw_plus_ultra.asm
; x86-64 Linux pomodoro timer — pure syscalls, no libc
;
; Syscalls used:
;   1  write(fd, buf, len)
;   35 nanosleep(req, rem)
;   60 exit(code)

; ── Constants ────────────────────────────────────────────────────────────────
SYS_WRITE    equ 1
SYS_NANOSLEEP equ 35
SYS_EXIT     equ 60
STDOUT       equ 1

section .rodata

help_msg:
    db "Usage: pomodoro [options]", 10
    db "Options:", 10
    db "  --focus-duration <min>      Focus duration in minutes (default: 25)", 10
    db "  --short-break <min>         Short break duration (default: 5)", 10
    db "  --long-break <min>          Long break duration (default: 15)", 10
    db "  --long-break-interval <n>   Sessions before long break (default: 4)", 10
    db "  --help                      Show this help", 10
help_msg_len equ $ - help_msg

str_help              db "--help", 0
str_focus_duration    db "--focus-duration", 0
str_short_break       db "--short-break", 0
str_long_break        db "--long-break", 0
str_long_break_intv   db "--long-break-interval", 0

; Session label strings (with " for " already appended for display)
lbl_focus  db "Focus session", 0
lbl_short  db "Short break", 0
lbl_long   db "Long break", 0

str_for    db " for ", 0
str_min    db " minute(s).", 10, 0
str_timeu  db " remaining...   ", 0   ; trailing spaces erase leftover digits

section .data
focus_duration   dd 25
short_break_dur  dd 5
long_break_dur   dd 15
long_break_intv  dd 4

section .bss
timespec  resq 2   ; [0]=tv_sec, [1]=tv_nsec  (16 bytes)

section .text
global _start

; ── Entry point ──────────────────────────────────────────────────────────────
_start:
    mov r12, [rsp]        ; r12 = argc
    mov r13, rsp          ; r13 = &argc (argv starts at r13+8)

    ; i = 1
    mov r14d, 1

.arg_loop:
    cmp r14d, r12d
    jge .run

    ; rbx = argv[i]  (rbx is callee-saved; save it here for streq reuse)
    lea rax, [r13 + r14*8 + 8]
    mov rbx, [rax]

    mov rdi, rbx
    lea rsi, [rel str_help]
    call streq
    test rax, rax
    jnz .do_help

    mov rdi, rbx
    lea rsi, [rel str_focus_duration]
    call streq
    test rax, rax
    jnz .do_focus_duration

    mov rdi, rbx
    lea rsi, [rel str_short_break]
    call streq
    test rax, rax
    jnz .do_short_break

    mov rdi, rbx
    lea rsi, [rel str_long_break]
    call streq
    test rax, rax
    jnz .do_long_break

    mov rdi, rbx
    lea rsi, [rel str_long_break_intv]
    call streq
    test rax, rax
    jnz .do_long_break_intv

    inc r14d
    jmp .arg_loop

    ; ── flag handlers (each reads argv[i+1], increments i by 2) ──

.do_help:
    mov rax, SYS_WRITE
    mov rdi, STDOUT
    lea rsi, [rel help_msg]
    mov rdx, help_msg_len
    syscall
    xor edi, edi
    mov eax, SYS_EXIT
    syscall

.do_focus_duration:
    inc r14d
    cmp r14d, r12d
    jge .run
    lea rax, [r13 + r14*8 + 8]
    mov rdi, [rax]
    call atoi
    mov [rel focus_duration], eax
    inc r14d
    jmp .arg_loop

.do_short_break:
    inc r14d
    cmp r14d, r12d
    jge .run
    lea rax, [r13 + r14*8 + 8]
    mov rdi, [rax]
    call atoi
    mov [rel short_break_dur], eax
    inc r14d
    jmp .arg_loop

.do_long_break:
    inc r14d
    cmp r14d, r12d
    jge .run
    lea rax, [r13 + r14*8 + 8]
    mov rdi, [rax]
    call atoi
    mov [rel long_break_dur], eax
    inc r14d
    jmp .arg_loop

.do_long_break_intv:
    inc r14d
    cmp r14d, r12d
    jge .run
    lea rax, [r13 + r14*8 + 8]
    mov rdi, [rax]
    call atoi
    mov [rel long_break_intv], eax
    inc r14d
    jmp .arg_loop

; ── Main pomodoro loop ───────────────────────────────────────────────────────
; r15d = completed focus sessions (starts at 0)
.run:
    xor r15d, r15d

.session_focus:
    lea rdi, [rel lbl_focus]
    mov esi, [rel focus_duration]
    call run_timer

    inc r15d

    ; if (r15d % long_break_intv == 0) → long break, else short break
    mov eax, r15d
    xor edx, edx
    div dword [rel long_break_intv]
    test edx, edx
    jnz .do_short

.do_long:
    lea rdi, [rel lbl_long]
    mov esi, [rel long_break_dur]
    call run_timer
    jmp .session_focus

.do_short:
    lea rdi, [rel lbl_short]
    mov esi, [rel short_break_dur]
    call run_timer
    jmp .session_focus

    ; (unreachable — loop is infinite like main_raw_plus)
    xor edi, edi
    mov eax, SYS_EXIT
    syscall

; ── run_timer(rdi=label_ptr, esi=duration_minutes) ──────────────────────────
; Prints "<label> for <N> minute(s).\n" then counts down second by second.
run_timer:
    push rbx
    push rbp
    push r12
    push r13

    mov rbx, rdi          ; rbx = label
    mov r12d, esi         ; r12d = duration in minutes

    ; Print header: "<label> for <minutes> minute(s).\n"
    mov rdi, rbx
    call write_str0
    lea rdi, [rel str_for]
    call write_str0
    mov edi, r12d         ; print minutes (before converting to seconds)
    call write_uint
    lea rdi, [rel str_min]
    call write_str0

    imul r12d, r12d, 60   ; r12d = total seconds (after printing minutes)

.tick:
    test r12d, r12d
    jz .done

    ; Write "\r<MM>:<SS> remaining...   "
    mov al, 0x0D          ; '\r'
    call write_char

    ; MM
    mov eax, r12d
    xor edx, edx
    mov ecx, 60
    div ecx               ; eax = minutes, edx = seconds
    mov ebp, edx          ; save seconds
    mov rdi, rax
    call write_uint2      ; always 2 digits

    mov al, ':'
    call write_char

    mov rdi, rbp
    call write_uint2      ; always 2 digits

    lea rdi, [rel str_timeu]
    call write_str0

    ; nanosleep(1 second)
    mov qword [rel timespec],     1   ; tv_sec
    mov qword [rel timespec + 8], 0   ; tv_nsec
    mov rax, SYS_NANOSLEEP
    lea rdi, [rel timespec]
    xor rsi, rsi
    syscall

    dec r12d
    jmp .tick

.done:
    ; Overwrite the countdown line with newline so prompt appears cleanly
    mov al, 10
    call write_char

    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; ── write_str0(rdi=NUL-terminated string) ────────────────────────────────────
write_str0:
    push rdi
    xor rcx, rcx
.ws_len:
    cmp byte [rdi + rcx], 0
    jz .ws_write
    inc rcx
    jmp .ws_len
.ws_write:
    ; rdi still = string ptr, rcx = length
    mov rdx, rcx
    mov rsi, rdi
    mov rdi, STDOUT
    mov rax, SYS_WRITE
    syscall
    pop rdi
    ret

; ── write_char(al=character) ─────────────────────────────────────────────────
write_char:
    push rax
    mov rax, SYS_WRITE
    mov rdi, STDOUT
    mov rsi, rsp
    mov rdx, 1
    syscall
    pop rax
    ret

; ── write_uint(rdi=uint32) — variable-width decimal ─────────────────────────
; Uses the stack as a scratch buffer (max 10 digits).
write_uint:
    push rbp
    sub rsp, 16           ; scratch buffer
    mov rbp, rsp

    mov eax, edi
    lea rsi, [rbp + 15]   ; end of buffer
    mov byte [rsi], 0
    mov ecx, 10

.wu_loop:
    xor edx, edx
    div ecx
    add dl, '0'
    dec rsi
    mov [rsi], dl
    test eax, eax
    jnz .wu_loop

    ; write from rsi to end of buffer
    lea rdx, [rbp + 15]
    sub rdx, rsi          ; length

    mov rdi, STDOUT
    mov rax, SYS_WRITE
    syscall

    add rsp, 16
    pop rbp
    ret

; ── write_uint2(rdi=uint32) — exactly 2 decimal digits (00–99) ──────────────
; Writes both digits in one syscall using a 2-byte buffer on the stack.
write_uint2:
    sub rsp, 8            ; 8-byte aligned scratch (use first 2 bytes)

    mov eax, edi
    xor edx, edx
    mov ecx, 10
    div ecx               ; eax = tens digit, edx = ones digit

    add al, '0'
    mov [rsp], al         ; tens
    add dl, '0'
    mov [rsp + 1], dl     ; ones

    mov rax, SYS_WRITE
    mov rdi, STDOUT
    mov rsi, rsp
    mov rdx, 2
    syscall

    add rsp, 8
    ret

; ── streq(rdi=s1, rsi=s2) → rax=1 if equal, 0 otherwise ────────────────────
streq:
    xor eax, eax
.se_loop:
    mov cl, [rdi]
    cmp cl, [rsi]
    jne .se_ret
    test cl, cl
    jz .se_equal
    inc rdi
    inc rsi
    jmp .se_loop
.se_equal:
    mov eax, 1
.se_ret:
    ret

; ── atoi(rdi=string) → eax=uint ─────────────────────────────────────────────
atoi:
    xor eax, eax
.ai_loop:
    movzx ecx, byte [rdi]
    sub ecx, '0'
    jl  .ai_done
    cmp ecx, 9
    jg  .ai_done
    imul eax, eax, 10
    add  eax, ecx
    inc  rdi
    jmp  .ai_loop
.ai_done:
    ret

