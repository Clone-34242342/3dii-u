"""A native-only restart need not start CafeGLSL to have working shaders."""
import re
from check_ports import SHADER_READY

assert re.search(SHADER_READY, '0 [glsl] compiler 1.0.0 up\n')
assert re.search(SHADER_READY, '0 [glsl] compiler thread ready\n')
assert re.search(SHADER_READY, '31 [glsl] pixel aabbccdd00112233 restored from native cache\n')
assert re.search(SHADER_READY, '31 [glsl] vertex 00112233aabbccdd restored from native cache\n')
assert not re.search(SHADER_READY, '[glsl] 20 shaders from the cache to restore or compile\n')
assert not re.search(SHADER_READY, '[glsl] pixel 00112233aabbccdd failed\n')
assert not re.search(SHADER_READY, '[glsl] compiler unavailable\n')
print('Shader readiness accepts verified native restores and compiler startup only')
