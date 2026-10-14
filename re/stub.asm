.code
stub proc
    push rbx
    sub rsp, 30h
    mov rbx, rcx
    lea rcx, [rbx+10h]
    call qword ptr [rbx]
    test rax, rax
    jz fail1
    mov rcx, rax
    lea rdx, [rbx+20h]
    call qword ptr [rbx+8]
    test rax, rax
    jz fail2
    lea rcx, [rbx+40h]
    lea rdx, [rbx+50h]
    lea r8, [rbx+460h]
    lea r9, [rbx+870h]
    lea r10, [rbx+0C80h]
    mov [rsp+20h], r10
    call rax
    jmp done
fail1:
    mov eax, 65h
    jmp done
fail2:
    mov eax, 66h
done:
    add rsp, 30h
    pop rbx
    ret
stub endp
end
