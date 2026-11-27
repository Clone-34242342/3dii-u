#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D faceTexture;
layout(binding = 0, std140) uniform Material {
    vec4 channels[3];
    vec4 mode;
};
void main() {
    if (mode.x == 0.0) {
        color = vec4(channels[0].rgb, 1.0);
        return;
    }
    vec4 t = texture(faceTexture, uv);
    if (mode.x == 1.0)
        color = t;
    else if (mode.x == 2.0)
        color = vec4(channels[0].rgb * t.r + channels[1].rgb * t.g + channels[2].rgb * t.b, t.a);
    else if (mode.x == 3.0)
        color = vec4(channels[0].rgb * t.r, t.r);
    else if (mode.x == 4.0)
        color = vec4(channels[0].rgb * t.g, t.r);
    else
        color = vec4(channels[0].rgb * t.r, 1.0);
}
