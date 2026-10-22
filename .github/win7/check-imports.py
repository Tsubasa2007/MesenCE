#Fails if a built file imports anything that isn't in imports.txt - the imports of a build known to
#run on Windows 7. A newer toolset or SDK can quietly start importing a function Windows 7 doesn't
#have (it happened with D3DCompiler_47.dll and CreateFile2), and the exe then won't start there.
#Usage: check-imports.py [--update] file...   (--update rewrites imports.txt from the given files)
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, 'imports.txt')

def dumpbin():
	vswhere = os.path.join(os.environ['ProgramFiles(x86)'], 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
	found = subprocess.run([vswhere, '-latest', '-products', '*', '-find', r'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe'],
		capture_output=True, text=True, check=True).stdout.splitlines()
	if not found:
		sys.exit('dumpbin.exe not found')
	return found[0]

def imports(tool, path):
	#Regular and delay-load imports, as "file: dll!function" (or dll!#ordinal)
	out = subprocess.run([tool, '/nologo', '/imports', path], capture_output=True, text=True, check=True).stdout
	name = os.path.basename(path)
	result = set()
	dll = None
	for line in out.splitlines():
		m = re.match(r'^    (\S+\.\w+)$', line)
		if m:
			dll = m.group(1).lower()
			continue
		if dll is None:
			continue
		m = re.match(r'^\s+(?:[0-9A-F]+\s+)+([A-Za-z_?@$][\w@?$]*)$', line)
		if m and m.group(1) != 'Characteristics':
			result.add(f'{name}: {dll}!{m.group(1)}')
			continue
		m = re.match(r'^\s+(?:[0-9A-F]+\s+)?Ordinal\s+(\d+)$', line)
		if m:
			result.add(f'{name}: {dll}!#{m.group(1)}')
	if not result:
		sys.exit(f'no imports read from {path}')
	return result

args = sys.argv[1:]
update = '--update' in args
files = [a for a in args if a != '--update']
tool = dumpbin()
found = set()
for f in files:
	found |= imports(tool, f)

if update:
	with open(LIST, 'w', newline='\n') as fh:
		fh.write('\n'.join(sorted(found)) + '\n')
	print(f'{LIST}: {len(found)} imports')
	sys.exit(0)

known = set(open(LIST).read().split('\n')) - {''}
new = sorted(found - known)
gone = sorted(i for i in known - found if i.split(':')[0] in {os.path.basename(f) for f in files})
for i in gone:
	print(f'no longer imported: {i}')
for i in new:
	print(f'::error::new import, check it exists on Windows 7: {i}')
print(f'{len(found)} imports, {len(new)} new, {len(gone)} no longer used')
sys.exit(1 if new else 0)
