#!/usr/bin/env python3
"""Run OXDK's oxdklink with autolink metadata cleared, without touching build products.

clang in MS compatibility mode records every #pragma comment(lib, ...) in a
.deplibs section -- including libc++.lib, which libc++'s own headers ask for.
-fno-autolink does not remove it and ld.lld honours the section both in objects
and inside static archives, so the link fails with "unable to find library from
dependent library specifier: libc++.lib".

Clearing that section in place is what the first attempt did, and it breaks the
build in a quieter way: objcopy rewrites the object, ninja sees an object newer
than its sources, and every incremental build recompiles the whole project. So
each input is copied into a staging directory, stripped there, and the link runs
against the copy. Build products keep their timestamps; the staged copies are
reused until the original changes.

    oxdklink_xbox360.py --objcopy <llvm-objcopy> --oxdklink <oxdklink.py>
        --stage <dir> <oxdklink arguments...> [-- <archives>]
"""

import hashlib
import os
import shutil
import subprocess
import sys


def parse_prefix(argv):
    """Pull this wrapper's own options off the front, the rest goes to oxdklink."""
    options = {'objcopy': None, 'oxdklink': None, 'stage': None}
    index = 0
    while index < len(argv):
        arg = argv[index]
        if arg.startswith('--') and arg[2:] in options:
            options[arg[2:]] = argv[index + 1]
            index += 2
            continue
        break
    missing = [name for name, value in options.items() if not value]
    if missing:
        sys.stderr.write('oxdklink_xbox360: missing option(s): '
                         + ', '.join('--' + name for name in missing) + '\n')
        sys.exit(2)
    return options, argv[index:]


def stage_file(path, stage, objcopy):
    """Copy one link input into the staging directory with .deplibs removed.

    The copy keeps the original's timestamp, so the work is skipped until the
    input changes. Stripping happens on the copy, never on the input.
    """
    source = os.path.abspath(path)
    try:
        source_stat = os.stat(source)
    except OSError:
        return path
    if not (source.endswith('.o') or source.endswith('.a') or source.endswith('.obj')):
        return path

    digest = hashlib.sha1(source.encode('utf-8', 'replace')).hexdigest()[:10]
    staged = os.path.join(stage, digest + '-' + os.path.basename(source))

    up_to_date = False
    if os.path.exists(staged):
        staged_stat = os.stat(staged)
        up_to_date = (staged_stat.st_size == source_stat.st_size
                      and int(staged_stat.st_mtime) == int(source_stat.st_mtime))
    if not up_to_date:
        shutil.copy2(source, staged)
        process = subprocess.run([objcopy, '--remove-section=.deplibs', staged],
                                 capture_output=True, text=True)
        if process.returncode != 0:
            # Not every input is ELF with sections to remove (librarian alias
            # members are machine 0x0000, for instance). A failure can also be
            # the result of an interrupted build (subprocess killed mid-run),
            # which must not leave an unstripped copy cached: delete it and
            # retry once so the next staging attempt starts from a clean copy.
            sys.stderr.write('oxdklink_xbox360: first strip of %s failed: %s'
                             % (path, process.stderr.strip()) + '\n')
            try:
                os.remove(staged)
            except OSError:
                pass
            shutil.copy2(source, staged)
            process = subprocess.run([objcopy, '--remove-section=.deplibs', staged],
                                     capture_output=True, text=True)
            if process.returncode != 0:
                sys.stderr.write('oxdklink_xbox360: keeping %s unstripped: %s'
                                 % (path, process.stderr.strip()) + '\n')
    # OXDK's builtins.c fills the gap between clang and the Microsoft C++
    # library with zeroed dummy vtables for four __cxxabiv1 typeinfo classes.
    # They make every dynamic_cast crash on the first virtual call. The real
    # definitions come from libc++abi's private_typeinfo.cpp (compiled into
    # xbox360_platform_shims), so remove the dummy definitions entirely. OXDK's
    # linker can still select a weak archive member before seeing the strong
    # definitions, so weakening alone is not sufficient. This must run even
    # when the staged archive is timestamp-current: older wrapper versions left
    # an unprocessed archive in the cache.
    if os.path.basename(source) == 'libxbox360_runtime.a' and os.path.exists(staged):
        strip_rtti = [objcopy]
        for dummy in ('_ZTVN10__cxxabiv116__enum_type_infoE',
                      '_ZTVN10__cxxabiv117__class_type_infoE',
                      '_ZTVN10__cxxabiv120__si_class_type_infoE',
                      '_ZTVN10__cxxabiv121__vmi_class_type_infoE',
                      '_ZTIi'):
            strip_rtti += ['--strip-symbol=' + dummy]
        strip_rtti.append(staged)
        process = subprocess.run(strip_rtti, capture_output=True, text=True)
        if process.returncode != 0:
            sys.stderr.write('oxdklink_xbox360: could not strip dummy RTTI vtables '
                             'in %s: %s' % (path, process.stderr.strip()) + '\n')
    if os.path.exists(staged):
        # Restore the source timestamp after all staging transforms so the copy
        # is recognised next time.
        os.utime(staged, (source_stat.st_atime, source_stat.st_mtime))
    return staged


def prune(stage, keep):
    """Drop staged copies whose original vanished from the link line."""
    for entry in os.listdir(stage):
        if entry in keep:
            continue
        full = os.path.join(stage, entry)
        if os.path.isfile(full):
            try:
                os.remove(full)
            except OSError:
                pass


def main():
    options, forwarded = parse_prefix(sys.argv[1:])
    stage = options['stage']
    os.makedirs(stage, exist_ok=True)

    rewritten, keep = [], set()
    for arg in forwarded:
        if arg.endswith('.o') or arg.endswith('.a') or arg.endswith('.obj'):
            staged = stage_file(arg, stage, options['objcopy'])
            if staged != arg:
                keep.add(os.path.basename(staged))
            rewritten.append(staged)
        else:
            rewritten.append(arg)
    prune(stage, keep)

    result = subprocess.run([sys.executable, options['oxdklink']] + rewritten)
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
