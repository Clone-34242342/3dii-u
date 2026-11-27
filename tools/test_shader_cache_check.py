"""A native-only restart need not start CafeGLSL to have working shaders."""
import re
from check_ports import SHADER_READY, categorize_fault, vertex_misses

assert re.search(SHADER_READY, '0 [glsl] compiler 1.0.0 up\n')
assert re.search(SHADER_READY, '0 [glsl] compiler thread ready\n')
assert re.search(SHADER_READY, '31 [glsl] pixel aabbccdd00112233 restored from native cache\n')
assert re.search(SHADER_READY, '31 [glsl] vertex 00112233aabbccdd restored from native cache\n')
assert not re.search(SHADER_READY, '[glsl] 20 shaders from the cache to restore or compile\n')
assert not re.search(SHADER_READY, '[glsl] pixel 00112233aabbccdd failed\n')
assert not re.search(SHADER_READY, '[glsl] compiler unavailable\n')
log = (
    '[gx2] no compiled vertex shader 0123456789abcdef, entry 4\n'
    '[glsl] vertex fedcba9876543210 compiled in 2 ms\n'
    '[glsl] pixel 0123456789abcdef restored from native cache\n'
)
assert vertex_misses(log) == ({'0123456789abcdef'}, set())
assert categorize_fault('blank save: 1 unresolved vertex shader miss(es): 0123456789abcdef') == 'shader fallback'
for result in ('compiled in 8 ms', 'restored from native cache'):
    assert vertex_misses(log + f'[glsl] vertex 0123456789abcdef {result}\n') == (set(), {'0123456789abcdef'})
print('Shader readiness accepts verified native restores and compiler startup only')
