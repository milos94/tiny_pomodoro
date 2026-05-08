; Pomodoro Timer — x86-64 Linux, NASM syntax
; Pure syscalls, no libc, no external dependencies.
;
; Sessions:  Focus (25 min) → Short Break (5 min) → ... every 4 pomodoros → Long Break (15 min)
; Controls:  s = start/pause   r = reset   q = quit
;            f/F = focus ±1    b/B = short break ±1   l/L = long break ±1
;
; The main loop sleeps 200 ms, redraws the display, and checks for keyboard input.
; A second thread counts down seconds using clone(2).

bits 64

; ── syscall numbers ──────────────────────────────────────────────────────────
%define SYS_read        0
%define SYS_write       1
%define SYS_open        2
%define SYS_close       3
%define SYS_exit        60
%define SYS_nanosleep   35
%define SYS_clone       56
%define SYS_ioctl       16
%define SYS_rt_sigaction 13
%define SYS_mmap        9
%define SYS_munmap      11

%define O_WRONLY        1

; ── clone flags for a new thread ─────────────────────────────────────────────
%define CLONE_VM        0x00000100
%define CLONE_FS        0x00000200
%define CLONE_FILES     0x00000400
%define CLONE_SIGHAND   0x00000800
%define CLONE_THREAD    0x00010000
%define CLONE_SYSVSEM   0x00040000
%define CLONE_SETTLS    0x00080000
%define CLONE_PARENT_SETTID 0x00100000
%define CLONE_CHILD_CLEARTID 0x00200000
%define THREAD_FLAGS    (CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM)

; ── mmap flags ───────────────────────────────────────────────────────────────
%define PROT_READ   1
%define PROT_WRITE  2
%define MAP_PRIVATE 2
%define MAP_ANON    0x20

; ── termios constants ─────────────────────────────────────────────────────────
%define TCGETS  0x5401
%define TCSETS  0x5402
%define ICANON  0x0002
%define ECHO    0x0008

; ── state constants ──────────────────────────────────────────────────────────
%define STATE_STOPPED  0
%define STATE_RUNNING  1
%define STATE_PAUSED   2

; ── session indices ──────────────────────────────────────────────────────────
%define SESS_FOCUS  0
%define SESS_SHORT  1
%define SESS_LONG   2

section .bss
    ; original termios (60 bytes is enough for struct termios on Linux)
    orig_termios:   resb 60
    raw_termios:    resb 60

    ; timer state (shared with countdown thread)
    g_state:        resd 1   ; STATE_*
    g_remaining:    resd 1   ; seconds left
    g_initial:      resd 1   ; total seconds for current session
    g_stop_thread:  resd 1   ; set to 1 to stop thread

    ; session info
    g_session:      resd 1   ; 0/1/2
    g_pomodoros:    resd 1

    ; duration settings (minutes)
    g_focus_min:    resd 1
    g_short_min:    resd 1
    g_long_min:     resd 1

    ; scratch buffer for number formatting
    itoa_buf:       resb 16

    ; output buffer
    out_buf:        resb 512

    ; fd for /dev/tty (for bell, opened at startup)
    g_tty_fd:       resd 1

    ; thread stack (1 MiB)
    thread_stack:   resb (1024 * 1024)
    thread_stack_end:

section .data
    ; ── string constants ─────────────────────────────────────────────────────
    str_clear       db 0x1b,'[2J',0x1b,'[H', 0
    str_home        db 0x1b,'[H', 0
    str_bold        db 0x1b,'[1m', 0
    str_reset       db 0x1b,'[0m', 0
    str_green       db 0x1b,'[32m', 0
    str_yellow      db 0x1b,'[33m', 0
    str_cursor_hide db 0x1b,'[?25l', 0
    str_cursor_show db 0x1b,'[?25h', 0
    str_newline     db 0x0a, 0
    str_bell        db 0x07, 0

    sess_focus      db 'Focus      ', 0
    sess_short      db 'Short Break', 0
    sess_long       db 'Long Break ', 0

    str_running     db 'Running', 0
    str_paused      db 'Paused ', 0

    str_sep         db '─────────────────────────────────', 0
    str_tl          db '┌', 0
    str_tr          db '┐', 0
    str_bl          db '└', 0
    str_br          db '┘', 0
    str_ml          db '├', 0
    str_mr          db '┤', 0
    str_pipe        db '│', 0
    str_colon       db ':', 0
    str_hash        db '#', 0
    str_space       db ' ', 0
    str_dot_g       db 0x1b,'[32m','●',0x1b,'[0m', 0
    str_dot_y       db 0x1b,'[33m','■',0x1b,'[0m', 0

    str_keys1       db '  [s] Start/Pause   [r] Reset    ', 0
    str_keys2       db '  [f/F] Focus-+     [b/B] Short-+', 0
    str_keys3       db '  [l/L] Long -+     [q]  Quit    ', 0

    str_err_tty     db 'error: requires a terminal', 0x0a, 0
    dev_tty         db '/dev/tty', 0

    ; 200 ms sleep timespec
    ts_200ms:       dq 0, 200000000
    ; 1 s sleep timespec
    ts_1s:          dq 1, 0
    ; 100 ms sleep timespec
    ts_100ms:       dq 0, 100000000

section .text
global _start

; ── helpers ──────────────────────────────────────────────────────────────────

; write(1, rdi, rsi)  — raw write syscall
sys_write:
    mov rax, SYS_write
    mov rdi, 1
    syscall
    ret

; print zero-terminated string at rdi
; clobbers rax, rcx, rdx, rsi
print_str:
    push rdi
    ; find length
    xor rcx, rcx
.len_loop:
    cmp byte [rdi + rcx], 0
    je .done_len
    inc rcx
    jmp .len_loop
.done_len:
    mov rsi, rdi
    mov rdx, rcx
    mov rax, SYS_write
    mov rdi, 1
    syscall
    pop rdi
    ret

; print single char in al
print_char:
    push rax
    mov rsi, rsp
    mov rdx, 1
    mov rax, SYS_write
    mov rdi, 1
    syscall
    pop rax
    ret

; convert dword in edi (0-9999) to decimal string in itoa_buf, null-terminated
; returns length in eax
itoa:
    push rbx
    mov eax, edi              ; save value FIRST, before rdi is clobbered
    test eax, eax
    jnz .nonzero
    lea rdi, [rel itoa_buf]
    mov byte [rdi], '0'
    mov byte [rdi+1], 0
    mov eax, 1
    pop rbx
    ret
.nonzero:
    ; build digits in reverse
    lea rbx, [rel itoa_buf + 14]
    mov byte [rbx], 0
    dec rbx
.div_loop:
    xor edx, edx
    mov ecx, 10
    div ecx           ; eax = quot, edx = remainder
    add dl, '0'
    mov [rbx], dl
    dec rbx
    test eax, eax
    jnz .div_loop
    ; rbx points one before first digit
    inc rbx
    ; copy to start of itoa_buf
    lea rdi, [rel itoa_buf]
    mov rsi, rbx
.copy_loop:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    test al, al
    jnz .copy_loop
    ; length
    lea rax, [rdi - 1]
    lea rcx, [rel itoa_buf]
    sub rax, rcx
    pop rbx
    ret

; print integer in edi with zero-padding to width in esi (1 or 2)
print_padded:
    push rsi
    call itoa           ; length in eax, string in itoa_buf
    pop rsi
    ; pad with '0' if needed
    cmp rsi, 2
    jne .no_pad
    cmp eax, 1
    jne .no_pad
    mov al, '0'
    call print_char
.no_pad:
    lea rdi, [rel itoa_buf]
    call print_str
    ret

; print a box row: │ + content (rdi) + │ + newline
; rdi = content string pointer
print_row:
    push rdi
    lea rdi, [rel str_pipe]
    call print_str
    pop rdi
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str
    ret

; ── terminal setup ────────────────────────────────────────────────────────────

setup_raw_terminal:
    ; TCGETS
    mov rax, SYS_ioctl
    mov rdi, 0          ; stdin
    mov rsi, TCGETS
    lea rdx, [rel orig_termios]
    syscall
    ; copy to raw_termios
    lea rsi, [rel orig_termios]
    lea rdi, [rel raw_termios]
    mov ecx, 60
    rep movsb
    ; clear ICANON and ECHO
    lea rdi, [rel raw_termios]
    mov eax, [rdi + 12]   ; c_lflag offset = 12 on Linux x86-64
    and eax, ~(ICANON | ECHO)
    mov [rdi + 12], eax
    ; VMIN=0 VTIME=0: c_cc starts at offset 17; VMIN=7, VTIME=6
    mov byte [rdi + 17 + 6], 0   ; VTIME
    mov byte [rdi + 17 + 7], 0   ; VMIN
    ; TCSETS
    mov rax, SYS_ioctl
    mov rdi, 0
    mov rsi, TCSETS
    lea rdx, [rel raw_termios]
    syscall
    ; hide cursor
    lea rdi, [rel str_cursor_hide]
    call print_str
    ret

restore_terminal:
    mov rax, SYS_ioctl
    mov rdi, 0
    mov rsi, TCSETS
    lea rdx, [rel orig_termios]
    syscall
    lea rdi, [rel str_cursor_show]
    call print_str
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_newline]
    call print_str
    ret

; ── countdown thread ─────────────────────────────────────────────────────────
; runs in a separate thread, decrements g_remaining every second

countdown_thread:
.loop:
    cmp dword [rel g_stop_thread], 1
    je .exit
    cmp dword [rel g_state], STATE_RUNNING
    jne .sleep_short
    ; sleep 1 second
    lea rdi, [rel ts_1s]
    xor rsi, rsi
    mov rax, SYS_nanosleep
    syscall
    cmp dword [rel g_state], STATE_RUNNING
    jne .loop
    mov eax, [rel g_remaining]
    test eax, eax
    jz .loop
    dec eax
    mov [rel g_remaining], eax
    test eax, eax
    jnz .loop
    ; timer expired → stop
    mov dword [rel g_state], STATE_STOPPED
    jmp .loop
.sleep_short:
    lea rdi, [rel ts_100ms]
    xor rsi, rsi
    mov rax, SYS_nanosleep
    syscall
    jmp .loop
.exit:
    mov rax, SYS_exit
    xor rdi, rdi
    syscall

; ── rendering ─────────────────────────────────────────────────────────────────

render:
    ; ── home cursor ──
    lea rdi, [rel str_home]
    call print_str

    ; top border  ┌──...──┐
    lea rdi, [rel str_tl]
    call print_str
    lea rdi, [rel str_sep]
    call print_str
    lea rdi, [rel str_tr]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; row: │  <session>          #<pomodoros>  │
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_bold]
    call print_str
    lea rdi, [rel str_space]
    call print_str
    lea rdi, [rel str_space]
    call print_str
    ; session name
    mov eax, [rel g_session]
    cmp eax, SESS_SHORT
    je .short_name
    cmp eax, SESS_LONG
    je .long_name
    lea rdi, [rel sess_focus]
    jmp .print_sname
.short_name:
    lea rdi, [rel sess_short]
    jmp .print_sname
.long_name:
    lea rdi, [rel sess_long]
.print_sname:
    call print_str
    ; spaces before #
    lea rdi, [rel str_space]
    call print_str
    call print_str
    call print_str
    call print_str
    call print_str
    call print_str
    call print_str
    call print_str
    lea rdi, [rel str_hash]
    call print_str
    mov edi, [rel g_pomodoros]
    mov esi, 1
    call print_padded
    ; padding to fill to pipe
    lea rdi, [rel str_space]
    call print_str
    call print_str
    call print_str
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; row: │         MM:SS                    │
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_bold]
    call print_str
    ; 9 spaces
    lea rdi, [rel str_space]
    %rep 9
    push rdi
    call print_str
    pop rdi
    %endrep
    lea rdi, [rel str_green]
    call print_str
    ; minutes
    mov eax, [rel g_remaining]
    xor edx, edx
    mov ecx, 60
    div ecx             ; eax = minutes, edx = seconds
    push rdx            ; save seconds — print_padded clobbers rdx
    mov edi, eax
    mov esi, 2
    call print_padded
    lea rdi, [rel str_colon]
    call print_str
    ; seconds
    pop rdx             ; restore seconds
    mov edi, edx
    mov esi, 2
    call print_padded
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_bold]
    call print_str
    ; trailing spaces + pipe
    lea rdi, [rel str_space]
    %rep 20
    push rdi
    call print_str
    pop rdi
    %endrep
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; row: │         ● Running/■ Paused       │
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_bold]
    call print_str
    lea rdi, [rel str_space]
    %rep 9
    push rdi
    call print_str
    pop rdi
    %endrep
    cmp dword [rel g_state], STATE_RUNNING
    je .running_dot
    lea rdi, [rel str_dot_y]
    call print_str
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_bold]
    call print_str
    lea rdi, [rel str_space]
    call print_str
    lea rdi, [rel str_paused]
    call print_str
    jmp .after_status
.running_dot:
    lea rdi, [rel str_dot_g]
    call print_str
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_bold]
    call print_str
    lea rdi, [rel str_space]
    call print_str
    lea rdi, [rel str_running]
    call print_str
.after_status:
    ; trailing spaces
    lea rdi, [rel str_space]
    %rep 16
    push rdi
    call print_str
    pop rdi
    %endrep
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; separator ├──...─┤
    lea rdi, [rel str_ml]
    call print_str
    lea rdi, [rel str_sep]
    call print_str
    lea rdi, [rel str_mr]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; row: │  Focus XX  Short XX  Long XX     │
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_bold]
    call print_str
    lea rdi, [rel str_space]
    call print_str
    call print_str
    ; "Focus "
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel sess_focus]
    call print_str      ; "Focus      " 11 chars — reuse, only prints "Focus      "
    ; Actually print our own shorter labels:
    ; (redefine inline below for brevity)
    ; We'll just print the values with manual spacing
    lea rdi, [rel str_bold]
    call print_str
    mov edi, [rel g_focus_min]
    mov esi, 2
    call print_padded
    lea rdi, [rel str_space]
    call print_str
    call print_str
    lea rdi, [rel str_reset]
    call print_str
    ; "Short "
    lea rdi, [rel str_bold]
    call print_str
    mov edi, [rel g_short_min]
    mov esi, 2
    call print_padded
    lea rdi, [rel str_space]
    call print_str
    call print_str
    lea rdi, [rel str_reset]
    call print_str
    ; "Long "
    lea rdi, [rel str_bold]
    call print_str
    mov edi, [rel g_long_min]
    mov esi, 2
    call print_padded
    lea rdi, [rel str_space]
    %rep 5
    push rdi
    call print_str
    pop rdi
    %endrep
    lea rdi, [rel str_reset]
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; separator ├──...─┤
    lea rdi, [rel str_ml]
    call print_str
    lea rdi, [rel str_sep]
    call print_str
    lea rdi, [rel str_mr]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; key rows
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_keys1]
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_keys2]
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_keys3]
    call print_str
    lea rdi, [rel str_pipe]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ; bottom border └──...──┘
    lea rdi, [rel str_bl]
    call print_str
    lea rdi, [rel str_sep]
    call print_str
    lea rdi, [rel str_br]
    call print_str
    lea rdi, [rel str_newline]
    call print_str

    ret

; ── set timer duration ────────────────────────────────────────────────────────
; edi = seconds
set_duration:
    mov [rel g_initial], edi
    mov [rel g_remaining], edi
    ret

; ── advance session ───────────────────────────────────────────────────────────
advance_session:
    ; ring bell via /dev/tty fd to ensure it is delivered
    mov rax, SYS_write
    mov edi, [rel g_tty_fd]
    lea rsi, [rel str_bell]
    mov rdx, 1
    syscall

    mov eax, [rel g_session]
    cmp eax, SESS_FOCUS
    jne .was_break

    ; was focus: increment pomodoro count
    mov eax, [rel g_pomodoros]
    inc eax
    mov [rel g_pomodoros], eax

    ; every 4th → long break, else short break
    xor edx, edx
    mov ecx, 4
    div ecx
    test edx, edx
    jnz .short_break
    ; long break
    mov dword [rel g_session], SESS_LONG
    mov edi, [rel g_long_min]
    imul edi, 60
    call set_duration
    jmp .start_next
.short_break:
    mov dword [rel g_session], SESS_SHORT
    mov edi, [rel g_short_min]
    imul edi, 60
    call set_duration
    jmp .start_next
.was_break:
    ; was break: go back to focus
    mov dword [rel g_session], SESS_FOCUS
    mov edi, [rel g_focus_min]
    imul edi, 60
    call set_duration
.start_next:
    mov dword [rel g_state], STATE_RUNNING
    ret

; ── main ──────────────────────────────────────────────────────────────────────

_start:
    ; ── check isatty(0) via ioctl TCGETS ─────────────────────────────────────
    mov rax, SYS_ioctl
    xor rdi, rdi
    mov rsi, TCGETS
    lea rdx, [rel orig_termios]
    syscall
    test rax, rax
    jns .is_tty
    lea rdi, [rel str_err_tty]
    call print_str
    mov rax, SYS_exit
    mov rdi, 1
    syscall
.is_tty:

    ; ── initialise state ─────────────────────────────────────────────────────
    mov dword [rel g_focus_min], 25
    mov dword [rel g_short_min], 5
    mov dword [rel g_long_min],  15
    mov dword [rel g_session],   SESS_FOCUS
    mov dword [rel g_pomodoros], 0
    mov dword [rel g_state],     STATE_STOPPED
    mov dword [rel g_stop_thread], 0

    ; g_initial = g_remaining = 25*60
    mov edi, 25 * 60
    call set_duration

    call setup_raw_terminal

    ; open /dev/tty for bell writes
    mov rax, SYS_open
    lea rdi, [rel dev_tty]
    mov rsi, O_WRONLY
    xor rdx, rdx
    syscall
    mov [rel g_tty_fd], eax

    ; clear screen
    lea rdi, [rel str_clear]
    call print_str

    ; ── spawn countdown thread ───────────────────────────────────────────────
    ; clone(THREAD_FLAGS, stack_top, NULL, NULL, NULL)
    mov rax, SYS_clone
    mov rdi, THREAD_FLAGS
    lea rsi, [rel thread_stack_end]   ; stack grows down, pass top
    xor rdx, rdx
    xor r10, r10
    xor r8, r8
    syscall
    test rax, rax
    jnz .parent        ; parent: rax = child tid
    ; child: call countdown_thread
    jmp countdown_thread
.parent:

    ; ── main loop ────────────────────────────────────────────────────────────
    xor r12d, r12d    ; was_running (0 = false)

.main_loop:
    ; non-blocking read of one char
    sub rsp, 8
    mov rax, SYS_read
    xor rdi, rdi
    lea rsi, [rsp]
    mov rdx, 1
    syscall
    mov cl, [rsp]
    add rsp, 8
    test rax, rax
    jle .no_key

    ; dispatch on key
    cmp cl, 's'
    je .key_s
    cmp cl, 'S'
    je .key_s
    cmp cl, 'r'
    je .key_r
    cmp cl, 'R'
    je .key_r
    cmp cl, 'f'
    je .key_f_lower
    cmp cl, 'F'
    je .key_f_upper
    cmp cl, 'b'
    je .key_b_lower
    cmp cl, 'B'
    je .key_b_upper
    cmp cl, 'l'
    je .key_l_lower
    cmp cl, 'L'
    je .key_l_upper
    cmp cl, 'q'
    je .quit
    cmp cl, 'Q'
    je .quit
    cmp cl, 0x1b
    je .quit
    jmp .no_key

.key_s:
    cmp dword [rel g_state], STATE_RUNNING
    je .pause_timer
    mov dword [rel g_state], STATE_RUNNING
    jmp .no_key
.pause_timer:
    mov dword [rel g_state], STATE_PAUSED
    jmp .no_key

.key_r:
    mov dword [rel g_state], STATE_STOPPED
    mov eax, [rel g_initial]
    mov [rel g_remaining], eax
    jmp .no_key

.key_f_lower:
    mov eax, [rel g_focus_min]
    cmp eax, 1
    jle .no_key
    dec eax
    mov [rel g_focus_min], eax
    ; if not running and in focus session, update timer
    cmp dword [rel g_state], STATE_RUNNING
    je .no_key
    cmp dword [rel g_session], SESS_FOCUS
    jne .no_key
    imul eax, 60
    mov edi, eax
    call set_duration
    jmp .no_key

.key_f_upper:
    mov eax, [rel g_focus_min]
    cmp eax, 240
    jge .no_key
    inc eax
    mov [rel g_focus_min], eax
    cmp dword [rel g_state], STATE_RUNNING
    je .no_key
    cmp dword [rel g_session], SESS_FOCUS
    jne .no_key
    imul eax, 60
    mov edi, eax
    call set_duration
    jmp .no_key

.key_b_lower:
    mov eax, [rel g_short_min]
    cmp eax, 1
    jle .no_key
    dec eax
    mov [rel g_short_min], eax
    jmp .no_key

.key_b_upper:
    mov eax, [rel g_short_min]
    cmp eax, 120
    jge .no_key
    inc eax
    mov [rel g_short_min], eax
    jmp .no_key

.key_l_lower:
    mov eax, [rel g_long_min]
    cmp eax, 1
    jle .no_key
    dec eax
    mov [rel g_long_min], eax
    jmp .no_key

.key_l_upper:
    mov eax, [rel g_long_min]
    cmp eax, 240
    jge .no_key
    inc eax
    mov [rel g_long_min], eax
    jmp .no_key

.no_key:
    ; ── check for session auto-advance ───────────────────────────────────────
    ; if was_running && !is_running && remaining == 0 → advance
    test r12d, r12d
    jz .skip_advance
    cmp dword [rel g_state], STATE_RUNNING
    je .skip_advance
    cmp dword [rel g_remaining], 0
    jne .skip_advance
    call advance_session
.skip_advance:

    ; update was_running
    xor r12d, r12d
    cmp dword [rel g_state], STATE_RUNNING
    jne .set_was
    mov r12d, 1
.set_was:

    call render

    ; sleep 200 ms
    lea rdi, [rel ts_200ms]
    xor rsi, rsi
    mov rax, SYS_nanosleep
    syscall

    jmp .main_loop

.quit:
    ; stop thread
    mov dword [rel g_stop_thread], 1
    ; close tty fd
    mov rax, SYS_close
    mov edi, [rel g_tty_fd]
    syscall
    call restore_terminal
    mov rax, SYS_exit
    xor rdi, rdi
    syscall
