#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 uv;
layout(binding = 0, std140) uniform Projection {
    mat4 transform;
};
void main() {
    gl_Position = transform * vec4(aPos, 1.0);
    uv = aUV;
}
