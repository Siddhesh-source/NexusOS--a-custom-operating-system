# user/: programs that run outside the kernel

Empty until Phase 6. Everything here runs in user mode (ring 3) and can
only reach the kernel through system calls.

Planned layout (create each folder when its phase starts):

```text
user/
  lib/        bharat.h: system-call wrappers, printf, malloc, why-rules, msg()   (Ph 7, 8, 12)
  shell/      bsh, the command shell                                         (Ph 7)
  programs/   hello, echo, cat, spin, crash, and user-mode test programs     (Ph 6, 7)
  services/   supervisor (restarts crashed services), desktop                (Ph 8, 11)
  apps/       Home, Terminal, Files, System ("Why?"), Permissions, Settings  (Ph 11)
  lang/       language packs: en.txt, hi.txt, mr.txt                        (Ph 12)
```

The build will pack the compiled programs into a startup bundle
(`build/initrd.tar`) that Limine loads next to the kernel. See
`docs/ROADMAP.md` Phase 7.
