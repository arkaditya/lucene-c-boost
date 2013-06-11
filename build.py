import os
import sys
import subprocess

try:
  import urllib.request
  urlopen = urllib.urlrequest.urlopen
except ImportError:
  import urllib2
  urlopen = urllib2.urlopen
  

# TODO
#  - other platforms

def getMavenJAR(groupID, artifactID, version):
  url = 'http://search.maven.org/remotecontent?filepath=%s/%s/%s/%s-%s.jar' % (groupID.replace('.', '/'), artifactID, version, artifactID, version)
  return urlopen(url).read()

def newer(first, second):
  """
  Returns True if first is newer than second.
  """
  if not os.path.exists(second):
    return true
  if type(first) is not list:
    first = [first]
  destModTime = os.path.getmtime(second)
  for x in first:
    if os.path.getmtime(x) > destModTime:
      return True
  return False

def run(cmd):
  print('  %s' % cmd)
  p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
  stdout, stderr = p.communicate()
  if p.returncode != 0:
    if type(stdout) is bytes:
      stdout = stdout.decode('utf-8')
    print('command "%s" failed:\n%s' % (cmd, stdout))
    raise RuntimeError()
  return stdout

def toClassPath(deps):
  l = []
  for tup in deps:
    l.append('lib/%s-%s.jar' % (tup[1], tup[2]))
  return ':'.join(l)

DEPS = (
  ('org.apache.lucene', 'lucene-core', '4.3.0'),)

TEST_DEPS = (
  ('junit', 'junit', '4.10'),
  ('org.apache.lucene', 'lucene-codecs', '4.3.0'),
  ('org.apache.lucene', 'lucene-test-framework', '4.3.0'),
  ('com.carrotsearch.randomizedtesting', 'junit4-ant', '2.0.9'),
  ('com.carrotsearch.randomizedtesting', 'randomizedtesting-runner', '2.0.9'))
  
if not os.path.exists('lib'):
  os.makedirs('lib')

for groupID, artifactID, version in DEPS + TEST_DEPS:
  fileName = 'lib/%s-%s.jar' % (artifactID, version)
  if not os.path.exists(fileName):
    print('Download %s-%s.jar' % (artifactID, version))
    jar = getMavenJAR(groupID, artifactID, version)
    open(fileName, 'wb').write(jar)

if not os.path.exists('build'):
  os.makedirs('build')

JAVA_HOME = os.environ['JAVA_HOME']

if not os.path.exists('dist'):
  os.makedirs('dist')
genPacked = 'src/c/org/apache/lucene/search/gen_Packed.py'
nativeSearch = 'src/c/org/apache/lucene/search/NativeSearch.cpp'
if newer(genPacked, nativeSearch):
  print('\nGenerated packed decode functions')
  run('%s %s' % (sys.executable, genPacked))

nativeSearchLib = 'dist/libNativeSearch.so'
if newer(nativeSearch, nativeSearchLib):
  # -ftree-vectorizer-verbose=3
  # -march=corei7
  print('\nCompile NativeSearch.cpp')
  run('g++ -fPIC -O4 -shared -o %s -I%s/include -I%s/include/linux %s' % (nativeSearchLib, JAVA_HOME, JAVA_HOME, nativeSearch))

mmapSource = 'src/c/org/apache/lucene/store/NativeMMapDirectory.cpp'
mmapLib = 'dist/libNativeMMapDirectory.so'
if newer(mmapSource, mmapLib):
  print('\nCompile NativeMMapDirectory.cpp')
  run('g++ -fPIC -O4 -shared -o %s -I%s/include -I%s/include/linux %s' % (mmapLib, JAVA_HOME, JAVA_HOME, mmapSource))

print('\nCompile java sources')
if not os.path.exists('build/classes/java'):
  os.makedirs('build/classes/java')
run('javac -XDignore.symbol.file -d build/classes/java -cp %s src/java/org/apache/lucene/store/*.java src/java/org/apache/lucene/search/*.java' % toClassPath(DEPS))
run('jar cf dist/luceneCBoost.jar -C build/classes/java .')

if not os.path.exists('build/classes/test'):
  os.makedirs('build/classes/test')

run('javac -d build/classes/test -cp %s:dist/luceneCBoost.jar src/test/org/apache/lucene/search/*.java' % toClassPath(DEPS + TEST_DEPS))

print('\nRun tests')
if not os.path.exists('build/test'):
  os.makedirs('build/test')

command = 'export LD_LIBRARY_PATH=%s/dist; java -Xmx128m -ea' % os.getcwd()
command += ' -cp %s:build/classes/test:dist/luceneCBoost.jar' % toClassPath(DEPS + TEST_DEPS)
command += ' -DtempDir=build/test'
command += ' -Dtests.codec=Lucene42'
command += ' -Dtests.directory=NativeMMapDirectory'
command += ' -Dtests.seed=0'
if len(sys.argv) != 1:
  command += ' -Dtests.method=%s' % sys.argv[1]
command += ' org.junit.runner.JUnitCore'
command += ' org.apache.lucene.search.TestNativeSearch'
p = subprocess.Popen(command, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

while True:
  s = p.stdout.readline()
  if s == b'':
    break
  print(s.decode('utf-8').rstrip())
p.wait()
if p.returncode != 0:
  raise RuntimeError('test failed')
