import sys
for p in sys.argv[1:]:
    d = open(p, 'rb').read().replace(b'\r\n', b'\n')
    open(p, 'wb').write(d)
print('converted', len(sys.argv) - 1)
