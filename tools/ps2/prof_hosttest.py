"""Exercise production profiler reporting beyond the 32-bit COP0 window limit."""
import argparse
from pathlib import Path
import math_common as C


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    text = (C.ROOT / 'src/ps2/ps2_prof.c').read_text()
    report = text[text.index('static void Report(void)'):text.index('#define WRAP_VOID0')]
    fixture = out / 'report.c'
    fixture.write_text('''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#define PP_COUNT 3
static UINT32 frames=105, tics=105, windows=1, last=0xfffffff0u;
static UINT64 acc[PP_COUNT]={100, 6000000000ull, 2000000000ull}, calls[PP_COUNT]={1,2,3};
static int cur=0;
static const char *names[PP_COUNT]={"other","render","audio"};
static char output[1000];
static UINT32 count(void) { return 0x30; }
static void FProf_Report(void) {}
static void I_OutputMsg(const char *format, ...) {
    va_list args; va_start(args,format);
    vsnprintf(output+strlen(output),sizeof output-strlen(output),format,args);
    va_end(args);
}
''' + report + '''
int main(void) {
    Report(); puts(output);
    if (!strstr(output,"total=8000000164") || acc[0] || acc[1] || acc[2]
        || calls[0] || calls[1] || calls[2] || frames || tics || windows!=2 || last!=0x30) return 1;
    puts("PASS wrapped Count interval, 64-bit window total and counter reset");
    return 0;
}
''')
    exe = C.msvc_build(out, 'prof_report', [(fixture, 'report.obj', [])], cl_extra=['/W4', '/WX'])
    rc, text = C.run(exe, log=out / 'test.log')
    print(text, end='')
    return rc


if __name__ == '__main__':
    raise SystemExit(main())
