#version 420 core

layout(location = 0) in vec2 v_texcoord;

layout(binding = 0) uniform sampler2D tex_y;
layout(binding = 1) uniform sampler2D tex_uv;

layout(location = 0) out vec4 out_colour;

void main()
{
    float y_sample = texture(tex_y, v_texcoord).r;
    vec2 uv = texture(tex_uv, v_texcoord).rg - vec2(0.5, 0.5);

    /*
     * Rec.709 limited range:
     * H.264 video values -> RGB.
     *
     * The expensive conversion that SDL performed for every pixel now
     * happens here, in the GPU.
     */
    float y = 1.16438356 * (y_sample - (16.0 / 255.0));

    float r = y + 1.79274107 * uv.y;
    float g = y - 0.21324861 * uv.x
                - 0.53290933 * uv.y;
    float b = y + 2.11240179 * uv.x;

    out_colour = vec4(r, g, b, 1.0);
}
