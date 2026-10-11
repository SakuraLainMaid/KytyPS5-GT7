#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// The clear color_clear_check.comp selected for a float target (drawn with as many vertices as it
// left: 3 when the metadata held a clear, none otherwise).

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Entry {
    uint  draw[4];
    uvec4 color;
};

layout(push_constant) uniform Push {
    uvec2 entry_address;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    Entry entry = Entry(pc.entry_address);
    out_color   = uintBitsToFloat(entry.color);
}
