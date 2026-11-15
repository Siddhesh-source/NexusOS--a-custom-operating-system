; The desktop's fonts (fonts.bin, made by scripts/make_fonts.py), linked
; in as read-only data. font.c checks the bytes before using them.
section .rodata
align 16
global font_blob, font_blob_end
font_blob:
    incbin "fonts.bin"
font_blob_end:
section .note.GNU-stack noalloc noexec nowrite progbits
