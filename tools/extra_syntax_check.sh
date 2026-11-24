#!/bin/sh
# Syntax-check every gen/extra/extra_recomp_*.c (same flags as the host build) and
# write all diagnostics to work/extra_syntax.log. Run inside the tm-hostdev container.
cd /w
FLAGS='-DRECOMP_LOCAL_REGS -DRECOMP_MEM_QUAL= -DRECOMP_STRICT_ICALL -DRECOMP_TLS=__thread -DRECOMP_X87_WINDOW -Ithird_party/pcrecomp/runtime/recomp32 -Igen -Igen/extra -O0 -w -fwrapv -fno-strict-aliasing -include runtime/include/guest_prelude.h -fsyntax-only -ferror-limit=0'
ls gen/extra/extra_recomp_[0-9]*.c | xargs -P 8 -I{} sh -c "clang $FLAGS {} 2>&1 | grep -E 'error' " > work/extra_syntax.log 2>&1
echo "errors: $(wc -l < work/extra_syntax.log)"
