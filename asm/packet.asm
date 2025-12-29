; Assembly-optimized packet header creation
; networking library
; Author: kanjore
; Date: 2025

section .text
    global create_packet_header_asm

; Function: create_packet_header_asm
; Parameters:
;   rdi: pointer to packet header (packet_header_t *header)
;   rsi: message type (uint8_t type)
;   rdx: sequence number (uint32_t sequence)
;   rcx: data length (uint32_t length)
; Returns:
;   None (modifies header in place)
create_packet_header_asm:
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ; Set magic number (0xDEADBEEF)
    mov rax, 0xDEADBEEF
    mov [rdi], eax
    
    ; Set message type
    mov [rdi + 4], sil

    ; Set sequence number
    mov [rdi + 5], edx

    ; Set data length
    mov [rdi + 9], ecx

    ; Set checksum (will be calculated later)
    mov dword [rdi + 13], 0

    ; Set timestamp (using RDTSC for high-resolution timing)
    rdtsc
    shl rdx, 32
    or rax, rdx
    mov [rdi + 17], rax

    ; Clear reserved bytes
    xor rax, rax
    mov [rdi + 25], rax
    mov byte [rdi + 31], 0
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; Function: encrypt_data_asm
; Parameters:
;   rdi: pointer to data (uint8_t *data)
;   rsi: length of data (size_t length)
;   rdx: encryption key (uint32_t key)
; Returns:
;   None (modifies data in place)
global encrypt_data_asm
encrypt_data_asm:
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ; Check if length is zero
    test rsi, rsi
    jz .done
    
    ; Set up loop variables
    mov rbx, rdi        ; rbx = data pointer
    mov rcx, rsi        ; rcx = remaining bytes
    mov r12, rdx        ; r12 = key
    
.process_byte:
    ; Get current byte
    mov al, [rbx]
    
    ; Simple XOR encryption with key rotation
    xor al, r12b
    
    ; Rotate key for next byte
    rol r12, 3
    
    ; Store encrypted byte back
    mov [rbx], al
    
    ; Move to next byte
    inc rbx
    dec rcx
    jnz .process_byte
    
.done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; Function: decrypt_data_asm
; Parameters:
;   rdi: pointer to data (uint8_t *data)
;   rsi: length of data (size_t length)
;   rdx: encryption key (uint32_t key)
; Returns:
;   None (modifies data in place)
global decrypt_data_asm
decrypt_data_asm:
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ; Check if length is zero
    test rsi, rsi
    jz .done
    
    ; Set up loop variables
    mov rbx, rdi        ; rbx = data pointer
    mov rcx, rsi        ; rcx = remaining bytes
    mov r12, rdx        ; r12 = key
    
    ; Calculate how many rotations occurred
    mov r13, rsi        ; r13 = length
    mov r14, r12        ; r14 = initial key
    
.calculate_final_key:
    rol r14, 3          ; Apply same rotation as encryption
    dec r13
    jnz .calculate_final_key
    
    ; Now decrypt from end to beginning with reverse rotation
    add rbx, rsi        ; rbx = end of data
    dec rbx             ; rbx = last byte
    
.decrypt_byte:
    ; Get current byte
    mov al, [rbx]
    
    ; Reverse XOR encryption with current key
    xor al, r14b
    
    ; Store decrypted byte back
    mov [rbx], al
    
    ; Reverse key rotation (rotate right by 3)
    ror r14, 3
    
    ; Move to previous byte
    dec rbx
    dec rcx
    jnz .decrypt_byte
    
.done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; Function: fast_memcpy_asm
; Parameters:
;   rdi: destination pointer (void *dest)
;   rsi: source pointer (const void *src)
;   rdx: number of bytes to copy (size_t n)
; Returns:
;   rdi: destination pointer
global fast_memcpy_asm
fast_memcpy_asm:
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ; Check if length is zero
    test rdx, rdx
    jz .done
    
    ; Set up loop variables
    mov rbx, rsi        ; rbx = source pointer
    mov rcx, rdi        ; rcx = destination pointer
    mov r12, rdx        ; r12 = remaining bytes
    
    ; Copy 8 bytes at a time when possible
.copy_8_bytes:
    cmp r12, 8
    jl .copy_remaining
    
    mov rax, [rbx]
    mov [rcx], rax
    add rbx, 8
    add rcx, 8
    sub r12, 8
    jmp .copy_8_bytes
    
.copy_remaining:
    ; Copy remaining bytes one at a time
    test r12, r12
    jz .done
    
    mov al, [rbx]
    mov [rcx], al
    inc rbx
    inc rcx
    dec r12
    jmp .copy_remaining
    
.done:
    mov rax, rdi        ; Return destination pointer
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; Function: fast_memset_asm
; Parameters:
;   rdi: destination pointer (void *s)
;   rsi: value to set (int c)
;   rdx: number of bytes to set (size_t n)
; Returns:
;   rdi: destination pointer
global fast_memset_asm
fast_memset_asm:
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ; Check if length is zero
    test rdx, rdx
    jz .done
    
    ; Set up loop variables
    mov rbx, rdi        ; rbx = destination pointer
    mov rcx, rdx        ; rcx = remaining bytes
    mov al, sil         ; al = value to set
    mov r12, rax        ; r12 = value (extended to 64-bit)
    
    ; Replicate value across 64-bit register
    mov r13, r12
    shl r13, 8
    or r12, r13
    mov r13, r12
    shl r13, 16
    or r12, r13
    mov r13, r12
    shl r13, 32
    or r12, r13
    
.set_8_bytes:
    cmp rcx, 8
    jl .set_remaining
    
    mov [rbx], r12
    add rbx, 8
    sub rcx, 8
    jmp .set_8_bytes
    
.set_remaining:
    test rcx, rcx
    jz .done
    
    mov [rbx], al
    inc rbx
    dec rcx
    jmp .set_remaining
    
.done:
    mov rax, rdi        ; Return destination pointer
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret
