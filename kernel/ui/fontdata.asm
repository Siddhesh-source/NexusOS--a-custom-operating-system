; The desktop's fonts (fonts.bin) and the Hindi/Marathi phrases
; (phrases.bin), both made by scripts/make_fonts.py, linked in as
; read-only data. font.c checks the bytes before using them.
section .rodata
align 16
global font_blob, font_blob_end, phrase_blob, phrase_blob_end
font_blob:
    incbin "fonts.bin"
font_blob_end:
align 16
phrase_blob:
    incbin "phrases.bin"
phrase_blob_end:
section .note.GNU-stack noalloc noexec nowrite progbits
