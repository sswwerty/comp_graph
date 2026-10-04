#include <veekay/veekay.hpp>
#include <imgui.h>
#include <vulkan/vulkan_core.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr size_t object_count = 2;
float radians(float x) { return x * pi / 180.0f; }
void check(VkResult r, const char* message) { if (r != VK_SUCCESS) throw std::runtime_error(message); }

struct Vertex { veekay::vec3 position, normal, color; };
struct SceneUniform { veekay::mat4 view_projection; };
struct ModelUniform { veekay::mat4 model, rotation; veekay::vec4 tint; };
static_assert(sizeof(SceneUniform) == 64);
static_assert(sizeof(ModelUniform) == 144);
struct Object {
    float position[3]{}, rotation[3]{}, scale[3]{1,1,1}, tint[3]{1,1,1};
    veekay::graphics::Buffer* uniform = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;
};
std::array<Object, object_count> objects{};
veekay::graphics::Buffer *vertices = nullptr, *indices = nullptr, *scene_buffer = nullptr;
VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE, model_layout = VK_NULL_HANDLE;
VkDescriptorSet scene_set = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;
uint32_t index_count = 0;
bool perspective = true, animate = true, vertex_colors = true;
int selected = 0;
float camera_distance = 5.5f, fov = 60.0f, ortho_size = 2.2f;
float phase = 0.0f, speed = 1.0f;
float radius[3]{0.65f, 0.45f, 0.55f};
float frequency[3]{1.0f, 2.0f, 1.5f};
float spin[3]{45.0f, 70.0f, 25.0f};

VkShaderModule load_shader(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Shader missing: build shaders and run from project root");
    const std::streamoff size = file.tellg();
    if (size <= 0 || size % 4 != 0) throw std::runtime_error("Invalid SPIR-V shader");
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    if (!file) throw std::runtime_error("Failed to read shader");
    VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = static_cast<size_t>(size), .pCode = code.data()};
    VkShaderModule result = VK_NULL_HANDLE;
    check(vkCreateShaderModule(veekay::app.vk_device, &info, nullptr, &result), "Shader module creation failed");
    return result;
}

veekay::mat4 rotation_matrix(const float angles[3]) {
    return veekay::mat4::rotation({0,0,1}, radians(angles[2])) *
           veekay::mat4::rotation({0,1,0}, radians(angles[1])) *
           veekay::mat4::rotation({1,0,0}, radians(angles[0]));
}
SceneUniform scene_data() {
    const float aspect = float(veekay::app.window_width) / float(veekay::app.window_height);
    constexpr float near = 0.1f, far = 100.0f;
    veekay::mat4 projection{};
    if (perspective) projection = veekay::mat4::projection(fov, aspect, near, far);
    else {
        projection[0][0] = 1.0f / (ortho_size * aspect);
        projection[1][1] = 1.0f / ortho_size;
        projection[2][2] = 1.0f / (far - near);
        projection[3][2] = -near / (far - near);
        projection[3][3] = 1.0f;
    }
    return {projection * veekay::mat4::translation({0,0,camera_distance})};
}
ModelUniform model_data(size_t i) {
    const Object& o = objects[i];
    float position[3]{o.position[0], o.position[1], o.position[2]};
    float angles[3]{o.rotation[0], o.rotation[1], o.rotation[2]};
    if (i == 0) {
        position[0] += radius[0] * std::cos(frequency[0] * phase);
        position[1] += radius[1] * std::sin(frequency[1] * phase);
        position[2] += radius[2] * std::sin(frequency[2] * phase);
        for (int axis = 0; axis < 3; ++axis) angles[axis] += spin[axis] * phase;
    }
    const auto rotation = rotation_matrix(angles);
    const auto model = veekay::mat4::translation({position[0],position[1],position[2]}) *
        rotation * veekay::mat4::scaling({o.scale[0],o.scale[1],o.scale[2]});
    return {model, rotation, {o.tint[0],o.tint[1],o.tint[2],vertex_colors ? 1.0f : 0.0f}};
}
void create_cube() {
    struct Face { veekay::vec3 normal, right, up; };
    const std::array<Face,6> faces{{
        {{ 1,0,0},{0,0,-1},{0,1,0}}, {{-1,0,0},{0,0,1},{0,1,0}},
        {{0, 1,0},{1,0,0},{0,0,-1}}, {{0,-1,0},{1,0,0},{0,0,1}},
        {{0,0, 1},{1,0,0},{0,1,0}}, {{0,0,-1},{-1,0,0},{0,1,0}}
    }};
    std::vector<Vertex> verts;
    std::vector<uint32_t> inds;
    for (const auto& f : faces) {
        const uint32_t first = static_cast<uint32_t>(verts.size());
        for (auto uv : std::array<std::array<float,2>,4>{{{-1,-1},{1,-1},{1,1},{-1,1}}}) {
            const auto p = (f.normal + f.right * uv[0] + f.up * uv[1]) * 0.5f;
            const veekay::vec3 color{0.2f+0.8f*(p.x+0.5f),
                                     0.2f+0.8f*(p.y+0.5f),
                                     0.2f+0.8f*(p.z+0.5f)};
            verts.push_back({p,f.normal,color});
        }
        for (uint32_t n : {0u,1u,2u,0u,2u,3u}) inds.push_back(first+n);
    }
    index_count = static_cast<uint32_t>(inds.size());
    vertices = new veekay::graphics::Buffer(verts.size()*sizeof(Vertex), verts.data(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indices = new veekay::graphics::Buffer(inds.size()*sizeof(uint32_t), inds.data(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}
void initialize(VkCommandBuffer) {
    VkDevice device = veekay::app.vk_device;
    vertex_shader = load_shader("shaders/cube.vert.spv");
    fragment_shader = load_shader("shaders/cube.frag.spv");
    create_cube();
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3};
    VkDescriptorPoolCreateInfo pool_info{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=3,.poolSizeCount=1,.pPoolSizes=&pool_size};
    check(vkCreateDescriptorPool(device,&pool_info,nullptr,&descriptor_pool),"Descriptor pool failed");
    VkDescriptorSetLayoutBinding binding{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo layout_info{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    check(vkCreateDescriptorSetLayout(device,&layout_info,nullptr,&scene_layout),"Scene layout failed");
    check(vkCreateDescriptorSetLayout(device,&layout_info,nullptr,&model_layout),"Model layout failed");
    const VkDescriptorSetLayout layouts[2]{scene_layout,model_layout};
    VkPipelineLayoutCreateInfo pl_info{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=2,.pSetLayouts=layouts};
    check(vkCreatePipelineLayout(device,&pl_info,nullptr,&pipeline_layout),"Pipeline layout failed");
    const VkPipelineShaderStageCreateInfo stages[2]{
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,
         .module=vertex_shader,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,
         .module=fragment_shader,.pName="main"}};
    VkVertexInputBindingDescription vb{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[3]{
        {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},
        {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},
        {2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,color)}};
    VkPipelineVertexInputStateCreateInfo vi{.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&vb,
        .vertexAttributeDescriptionCount=3,.pVertexAttributeDescriptions=attrs};
    VkPipelineInputAssemblyStateCreateInfo ia{.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp{.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs{.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,
        .frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
    VkPipelineMultisampleStateCreateInfo ms{.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds{.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState attachment{.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|
        VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb{.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment};
    const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=2,.pDynamicStates=dynamic_states};
    VkGraphicsPipelineCreateInfo gp{.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,
        .pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,
        .pDepthStencilState=&ds,.pColorBlendState=&cb,.pDynamicState=&dyn,
        .layout=pipeline_layout,.renderPass=veekay::app.vk_render_pass};
    check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline),"Graphics pipeline failed");
    scene_buffer = new veekay::graphics::Buffer(sizeof(SceneUniform),nullptr,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    objects[0].tint[0]=0.95f; objects[0].tint[1]=0.85f;
    objects[0].rotation[0]=20.0f; objects[0].rotation[1]=25.0f;
    objects[1].position[0]=1.55f;
    for (float& component : objects[1].scale) component=0.55f;
    objects[1].rotation[1]=25.0f;
    objects[1].tint[0]=0.55f; objects[1].tint[1]=0.85f;
    for (auto& object : objects)
        object.uniform=new veekay::graphics::Buffer(sizeof(ModelUniform),nullptr,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const VkDescriptorSetLayout set_layouts[3]{scene_layout,model_layout,model_layout};
    VkDescriptorSet sets[3]{};
    VkDescriptorSetAllocateInfo alloc{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptor_pool,.descriptorSetCount=3,.pSetLayouts=set_layouts};
    check(vkAllocateDescriptorSets(device,&alloc,sets),"Descriptor allocation failed");
    scene_set=sets[0];
    for (size_t i=0;i<object_count;++i) objects[i].set=sets[i+1];
    VkDescriptorBufferInfo buffer_info[3]{{scene_buffer->buffer,0,sizeof(SceneUniform)},
        {objects[0].uniform->buffer,0,sizeof(ModelUniform)},
        {objects[1].uniform->buffer,0,sizeof(ModelUniform)}};
    VkWriteDescriptorSet writes[3]{};
    for (size_t i=0;i<3;++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet=sets[i];
        writes[i].dstBinding=0;
        writes[i].descriptorCount=1;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo=&buffer_info[i];
    }
    vkUpdateDescriptorSets(device,3,writes,0,nullptr);
}
void shutdown() {
    VkDevice device=veekay::app.vk_device;
    for (auto& o:objects) delete o.uniform;
    delete scene_buffer; delete indices; delete vertices;
    vkDestroyPipeline(device,pipeline,nullptr);
    vkDestroyPipelineLayout(device,pipeline_layout,nullptr);
    vkDestroyDescriptorPool(device,descriptor_pool,nullptr);
    vkDestroyDescriptorSetLayout(device,model_layout,nullptr);
    vkDestroyDescriptorSetLayout(device,scene_layout,nullptr);
    vkDestroyShaderModule(device,fragment_shader,nullptr);
    vkDestroyShaderModule(device,vertex_shader,nullptr);
}
void update(double time) {
    static double previous=time;
    const float dt=static_cast<float>(std::fmax(0.0,std::fmin(time-previous,0.1)));
    previous=time;
    if (animate) phase+=speed*dt;
    ImGui::Begin("Lab 1 - regular hexahedron (variant 12)");
    ImGui::Checkbox("Perspective projection",&perspective);
    if (perspective) ImGui::SliderFloat("Field of view",&fov,25.0f,100.0f);
    else ImGui::SliderFloat("Orthographic size",&ortho_size,1.0f,5.0f);
    ImGui::SliderFloat("Camera distance",&camera_distance,3.0f,12.0f);
    ImGui::Separator();
    ImGui::Combo("Object",&selected,"Animated cube\0Second cube\0");
    Object& o=objects[static_cast<size_t>(selected)];
    ImGui::DragFloat3("Position",o.position,0.02f,-5.0f,5.0f);
    ImGui::DragFloat3("Rotation (degrees)",o.rotation,0.5f,-360.0f,360.0f);
    ImGui::DragFloat3("Scale",o.scale,0.01f,0.1f,3.0f);
    ImGui::ColorEdit3("Object color",o.tint);
    ImGui::Checkbox("Procedural vertex colors",&vertex_colors);
    ImGui::Separator();
    ImGui::Checkbox("Play animation",&animate);
    ImGui::SliderFloat("Animation speed",&speed,-3.0f,3.0f);
    ImGui::DragFloat3("Orbit radii",radius,0.01f,0.0f,2.0f);
    ImGui::DragFloat3("Orbit frequencies",frequency,0.01f,0.0f,5.0f);
    ImGui::DragFloat3("Spin speed (deg/s)",spin,0.5f,-180.0f,180.0f);
    if (ImGui::Button("Reset animation phase")) phase=0.0f;
    ImGui::TextUnformatted("The first cube follows a 3D harmonic orbit and spins.");
    ImGui::End();
}
void render(VkCommandBuffer cmd,VkFramebuffer framebuffer) {
    // Starter frames share mapped buffers. Finish old reads before changing them.
    check(vkDeviceWaitIdle(veekay::app.vk_device),"Device wait failed");
    const SceneUniform scene=scene_data();
    std::memcpy(scene_buffer->mapped_region,&scene,sizeof(scene));
    for (size_t i=0;i<object_count;++i) {
        const ModelUniform model=model_data(i);
        std::memcpy(objects[i].uniform->mapped_region,&model,sizeof(model));
    }
    check(vkResetCommandBuffer(cmd,0),"Command buffer reset failed");
    VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    check(vkBeginCommandBuffer(cmd,&begin),"Command buffer begin failed");
    const VkClearValue clear[2]{{.color={{0.07f,0.09f,0.14f,1.0f}}},{.depthStencil={1.0f,0}}};
    VkRenderPassBeginInfo pass{.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass=veekay::app.vk_render_pass,.framebuffer=framebuffer,
        .renderArea={.extent={veekay::app.window_width,veekay::app.window_height}},
        .clearValueCount=2,.pClearValues=clear};
    vkCmdBeginRenderPass(cmd,&pass,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0,0,float(veekay::app.window_width),float(veekay::app.window_height),0,1};
    VkRect2D scissor{{0,0},{veekay::app.window_width,veekay::app.window_height}};
    vkCmdSetViewport(cmd,0,1,&viewport);
    vkCmdSetScissor(cmd,0,1,&scissor);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    const VkDeviceSize zero=0;
    const VkBuffer vertex=vertices->buffer;
    vkCmdBindVertexBuffers(cmd,0,1,&vertex,&zero);
    vkCmdBindIndexBuffer(cmd,indices->buffer,0,VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&scene_set,0,nullptr);
    for (const auto& o:objects) {
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,1,1,&o.set,0,nullptr);
        vkCmdDrawIndexed(cmd,index_count,1,0,0,0);
    }
    vkCmdEndRenderPass(cmd);
    check(vkEndCommandBuffer(cmd),"Command buffer end failed");
}
}
int main() { return veekay::run({.init=initialize,.shutdown=shutdown,.update=update,.render=render}); }
