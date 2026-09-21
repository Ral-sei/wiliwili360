import re
t = open('/tmp/flags94.txt').read()
for p in sorted(set(re.findall(r'-isystem(\S+)', t))):
    print('ISYSTEM', p)
for p in sorted(set(re.findall(r'(?:^|\s)-I\s*(\S+)', t))):
    print('I', p)
print('---')
print('bili360:', 'bili360' in t, '| cpr:', 'cpr' in t, '| curl:', 'curl' in t)
