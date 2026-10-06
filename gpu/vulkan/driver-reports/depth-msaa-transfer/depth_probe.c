/* Original R-comp Vulkan probe; no title assets or generated guest code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Exit 0 PASS, 1 FAIL, 2 BLOCKED; host execution never proves PS5.
 */
#include "rcvk_loader.h"
#include "oracle.h"
#include "depth_probe_spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
static jmp_buf abort_point;
#define STAGES_DS (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define ACCESS_DS (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
#define V(call) do { VkResult vr=(call); if(vr!=VK_SUCCESS) failure(#call,vr,__LINE__); } while(0)
struct Image { VkImage image; VkDeviceMemory memory; VkImageView both,depth,stencil; VkImageLayout layout; uint32_t width,samples; VkImageAspectFlags aspects; };
struct Buffer { VkBuffer buffer; VkDeviceMemory memory; void *map; VkDeviceSize size; };
struct Push { uint32_t mode,width,height,samples; float yscale,yoffset,xoffset; uint32_t tag; };
struct Sample { uint32_t depth,stencil,color,reserved; };
_Static_assert(sizeof(struct Push)==32,"push constant ABI");
_Static_assert(sizeof(struct Sample)==16,"std430 uvec4 ABI");
static uint8_t *composite;
static VkInstance instance; static VkPhysicalDevice physical; static VkDevice device; static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memories;
static VkCommandPool command_pool; static VkCommandBuffer command; static VkFence fence;
static VkDescriptorSetLayout descriptor_layout; static VkDescriptorPool descriptor_pool; static VkDescriptorSet descriptor_set; static VkPipelineLayout pipeline_layout; static VkSampler sampler;
static VkRenderPass source_pass,dest_pass,color_pass; static VkFramebuffer source_fb,dest_fb,color_fb;
static VkPipeline source_fill,dest_fill,transfer_pipeline,opaque_pipeline,compute_pipeline;
static struct Image source,dest,color,resolved; static struct Buffer samples,rgba; static unsigned mismatches;
static PFN_vkGetPhysicalDeviceImageFormatProperties image_format_properties;
static PFN_vkCreateComputePipelines create_compute_pipelines;
static PFN_vkCreateSampler create_sampler; static PFN_vkDestroySampler destroy_sampler;
static PFN_vkCmdDispatch dispatch; static PFN_vkCmdPushConstants push_constants; static PFN_vkCmdClearAttachments clear_attachments;
static void failure(const char *call,VkResult result,int line) {
    fprintf(stderr,"FAIL depth-probe Vulkan line=%d %s result=%d (%s)\n",line,call,(int)result,rcvk_result_name(result));
    fflush(stdout); fflush(stderr); longjmp(abort_point,1);
}
static void blocked(const char *reason) { fprintf(stderr,"BLOCKED depth-probe %s\n",reason); fflush(stderr); longjmp(abort_point,2); }
static uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memories.memoryTypeCount;++i) if((bits&(1u<<i)) && (memories.memoryTypes[i].propertyFlags&flags)==flags) return i;
    blocked("required memory type unavailable"); return 0;
}
static struct Buffer buffer_create(VkDeviceSize size,VkBufferUsageFlags usage) {
    struct Buffer b={.size=size};
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    V(rc_vkCreateBuffer(device,&ci,NULL,&b.buffer)); VkMemoryRequirements req; rc_vkGetBufferMemoryRequirements(device,b.buffer,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    V(rc_vkAllocateMemory(device,&ai,NULL,&b.memory)); V(rc_vkBindBufferMemory(device,b.buffer,b.memory,0)); V(rc_vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&b.map)); return b;
}
static VkImageView image_view(struct Image *image,VkFormat format,VkImageAspectFlags aspects) {
    VkImageView view;
    VkImageViewCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={.aspectMask=aspects,.levelCount=1,.layerCount=1}};
    V(rc_vkCreateImageView(device,&ci,NULL,&view)); return view;
}
static struct Image image_create(uint32_t width,VkFormat format,VkSampleCountFlagBits count,VkImageUsageFlags usage,VkImageAspectFlags aspects) {
    VkImageFormatProperties support; VkResult supported=image_format_properties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&support);
    if(supported!=VK_SUCCESS || !(support.sampleCounts&count) || support.maxExtent.width<width || support.maxExtent.height<DP_H) {
        fprintf(stderr,"depth-probe format=%u width=%u height=%u requestedSamples=%u usage=%X queryResult=%d supportedSamples=%X\n",(unsigned)format,width,DP_H,(unsigned)count,usage,(int)supported,supported==VK_SUCCESS?support.sampleCounts:0);
        blocked("required format/usage/MSAA image combination unavailable");
    }
    struct Image image={.layout=VK_IMAGE_LAYOUT_UNDEFINED,.width=width,.samples=(uint32_t)count,.aspects=aspects};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,.extent={width,DP_H,1},.mipLevels=1,.arrayLayers=1,.samples=count,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    V(rc_vkCreateImage(device,&ci,NULL,&image.image)); VkMemoryRequirements req; rc_vkGetImageMemoryRequirements(device,image.image,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    V(rc_vkAllocateMemory(device,&ai,NULL,&image.memory)); V(rc_vkBindImageMemory(device,image.image,image.memory,0)); image.both=image_view(&image,format,aspects);
    if(aspects&VK_IMAGE_ASPECT_DEPTH_BIT) { image.depth=image_view(&image,format,VK_IMAGE_ASPECT_DEPTH_BIT); image.stencil=image_view(&image,format,VK_IMAGE_ASPECT_STENCIL_BIT); } return image;
}
static VkShaderModule shader(const uint32_t *words,size_t size) {
    VkShaderModule module; VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=words}; V(rc_vkCreateShaderModule(device,&ci,NULL,&module)); return module;
}
static VkRenderPass render_pass(VkSampleCountFlagBits count,int with_color) {
    VkAttachmentDescription attachments[3]={{.format=VK_FORMAT_D32_SFLOAT_S8_UINT,.samples=count,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference ds={0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}, cr={1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, rr={2,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    if(with_color) {
        attachments[1]=(VkAttachmentDescription){.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=count,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        attachments[2]=(VkAttachmentDescription){.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=with_color?1u:0u,.pColorAttachments=with_color?&cr:NULL,.pResolveAttachments=with_color?&rr:NULL,.pDepthStencilAttachment=&ds};
    VkRenderPassCreateInfo ci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=with_color?3u:1u,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&sub}; VkRenderPass pass; V(rc_vkCreateRenderPass(device,&ci,NULL,&pass)); return pass;
}
static VkFramebuffer framebuffer(VkRenderPass pass,struct Image *depth,int with_color) {
    VkImageView views[3]={depth->both,color.both,resolved.both};
    VkFramebufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=with_color?3u:1u,.pAttachments=views,.width=depth->width,.height=DP_H,.layers=1}; VkFramebuffer fb; V(rc_vkCreateFramebuffer(device,&ci,NULL,&fb)); return fb;
}
static VkPipeline graphics_pipeline(VkRenderPass pass,VkSampleCountFlagBits count,int transfer,int opaque) {
    VkShaderModule vert=shader(quad_vert_spv,sizeof(quad_vert_spv)),frag=transfer?shader(transfer_frag_spv,sizeof(transfer_frag_spv)):shader(fill_frag_spv,sizeof(fill_frag_spv));
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=frag,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=count,.sampleShadingEnable=VK_TRUE,.minSampleShading=1};
    VkStencilOpState stencil={.failOp=VK_STENCIL_OP_KEEP,.passOp=VK_STENCIL_OP_REPLACE,.depthFailOp=transfer?VK_STENCIL_OP_REPLACE:VK_STENCIL_OP_KEEP,.compareOp=VK_COMPARE_OP_ALWAYS,.compareMask=255,.writeMask=255,.reference=0};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=transfer?VK_COMPARE_OP_NOT_EQUAL:(opaque?VK_COMPARE_OP_GREATER_OR_EQUAL:VK_COMPARE_OP_ALWAYS),.stencilTestEnable=VK_TRUE,.front=stencil,.back=stencil};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15}; VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=opaque?1u:0u,.pAttachments=opaque?&blend:NULL}; VkDynamicState states[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
    VkGraphicsPipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pDepthStencilState=&ds,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=pipeline_layout,.renderPass=pass,.subpass=0,.basePipelineIndex=-1};
    VkPipeline pipeline; V(rc_vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline)); rc_vkDestroyShaderModule(device,vert,NULL); rc_vkDestroyShaderModule(device,frag,NULL); return pipeline;
}
static void begin(void) {
    V(rc_vkResetCommandBuffer(command,0)); VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; V(rc_vkBeginCommandBuffer(command,&bi));
}
static void finish(void) {
    V(rc_vkEndCommandBuffer(command)); V(rc_vkResetFences(device,1,&fence)); VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command}; V(rc_vkQueueSubmit(queue,1,&si,fence));
    /* Submitted-error paths terminate before resources can be released. */
    V(rc_vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(60000000000)));
}
static void transition(struct Image *image,VkImageLayout layout,VkPipelineStageFlags stage,VkAccessFlags access) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=image->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=access,.oldLayout=image->layout,.newLayout=layout,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image->image,.subresourceRange={.aspectMask=image->aspects,.levelCount=1,.layerCount=1}};
    rc_vkCmdPipelineBarrier(command,image->layout==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,stage,0,0,NULL,0,NULL,1,&b); image->layout=layout;
}
static struct Push push(uint32_t mode,struct Image *image) { return (struct Push){.mode=mode,.width=image->width,.height=DP_H,.samples=image->samples,.yscale=1}; }
static void draw(VkRenderPass pass,VkFramebuffer fb,VkPipeline pipeline,struct Image *image,struct Push *pc,uint32_t viewport_height,uint32_t scissor_height,int color_enabled) {
    transition(image,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,STAGES_DS,ACCESS_DS);
    if(color_enabled) { transition(&color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT); transition(&resolved,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT); }
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{image->width,DP_H}}};
    rc_vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE); rc_vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    VkViewport vp={.width=(float)image->width,.height=(float)viewport_height,.minDepth=0,.maxDepth=1}; VkRect2D sc={{0,0},{image->width,scissor_height}};
    rc_vkCmdSetViewport(command,0,1,&vp); rc_vkCmdSetScissor(command,0,1,&sc);
    push_constants(command,pipeline_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(*pc),pc);
    rc_vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&descriptor_set,0,NULL); rc_vkCmdDraw(command,6,1,0,0); rc_vkCmdEndRenderPass(command);
}
/* Two immutable sets: never rewrite a recorded transfer's source descriptors. */
static VkDescriptorSet source_set,dest_set;
static void descriptors(struct Image *image) { descriptor_set=image==&source?source_set:dest_set; }
static void write_descriptors(VkDescriptorSet set,struct Image *image) {
    VkDescriptorImageInfo images[3]={{sampler,image->depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,image->stencil,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,color.both,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorBufferInfo buffer={samples.buffer,0,samples.size}; VkWriteDescriptorSet writes[4]; memset(writes,0,sizeof(writes));
    for(uint32_t i=0;i<4;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=i==3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[i].pImageInfo=i<3?&images[i]:NULL; writes[i].pBufferInfo=i==3?&buffer:NULL; }
    rc_vkUpdateDescriptorSets(device,4,writes,0,NULL);
}
static void snapshot(struct Image *image,int with_color) {
    /* Poison all output words: a stale/unwritten snapshot cannot pass. */
    memset(samples.map,0xCD,(size_t)samples.size);
    VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=samples.memory,.size=VK_WHOLE_SIZE};
    V(rc_vkFlushMappedMemoryRanges(device,1,&flush));
    descriptors(image); transition(image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    transition(&color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    VkBufferMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=samples.buffer,.size=VK_WHOLE_SIZE};
    rc_vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&before,0,NULL);
    rc_vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,compute_pipeline); rc_vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&descriptor_set,0,NULL);
    struct Push pc=push(with_color?1u:0u,image); push_constants(command,pipeline_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc); dispatch(command,(image->width+7)/8,(DP_H+7)/8,1);
    VkBufferMemoryBarrier after=before; after.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    rc_vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&after,0,NULL);
    if(with_color) {
        transition(&resolved,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={DP_W,DP_H,1}};
        /* Only the SINGLE-SAMPLE resolve image is copied to a buffer. */
        rc_vkCmdCopyImageToBuffer(command,resolved.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rgba.buffer,1,&copy);
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=rgba.buffer,.size=VK_WHOLE_SIZE};
        rc_vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    }
    finish(); VkMappedMemoryRange ranges[2]={{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=samples.memory,.size=VK_WHOLE_SIZE},{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=rgba.memory,.size=VK_WHOLE_SIZE}}; V(rc_vkInvalidateMappedMemoryRanges(device,with_color?2u:1u,ranges));
}
static void mismatch(const char *label,uint32_t x,uint32_t y,uint32_t sample_id,const char *aspect,uint32_t got,uint32_t expected) {
    if(mismatches<16) fprintf(stderr,"FAIL %s (%u,%u) sample%u %s got=%08X expected=%08X\n",label,x,y,sample_id,aspect,got,expected);
    ++mismatches;
}
/* Modes: source0, patterned import1, equal-depth import2, constant import3,
 * opaque4, clear5. All pixels/samples, including outside each scissor, checked. */
static void compare(const char *label,struct Image *image,uint32_t mode,uint32_t height,uint32_t tile) {
    struct Sample *data=samples.map; unsigned before=mismatches;
    uint64_t observed=UINT64_C(14695981039346656037),expected_hash=observed;
    for(uint32_t y=0;y<DP_H;++y) for(uint32_t x=0;x<image->width;++x) for(uint32_t s=0;s<image->samples;++s) {
        uint32_t ss=image==&source?s:dp_source_sample(x,s),sx=image==&source?x:x>>1;
        float depth=dp_pattern_depth(sx,y,ss); uint32_t stencil=dp_pattern_stencil(sx,y,ss),expected_color=0;
        if(mode==1 && y>=height) { depth=0.75f; stencil=165; }
        if(mode==2 && y>=height) stencil=165;
        if(mode>=3) {
            depth=y<256?0.25f:0.75f; stencil=y<256?3u:165u;
            if(mode==4) { depth=y<height?0.25f:(y<256?0.0f:0.75f); stencil=y<height?(tile==0?0x82u:(tile==1?0x81u:8u)):(y<256?0u:165u); expected_color=y<height?dp_color(tile):0; }
            if(mode==5) { depth=y<256?0.0f:0.75f; stencil=y<256?0u:165u; }
        }
        size_t i=((size_t)y*image->width+x)*image->samples+s; struct Sample got=data[i]; uint32_t expected_depth=dp_float_bits(depth);
        uint32_t actual[4]={got.depth,got.stencil,got.color,got.reserved},expect[4]={expected_depth,stencil,expected_color,0};
        for(unsigned word=0;word<4;++word) for(unsigned byte=0;byte<4;++byte) {
            observed=(observed^((actual[word]>>(8*byte))&255u))*UINT64_C(1099511628211);
            expected_hash=(expected_hash^((expect[word]>>(8*byte))&255u))*UINT64_C(1099511628211);
        }
        if(got.reserved) mismatch(label,x,y,s,"written-record",got.reserved,0);
        if(got.depth!=expected_depth) mismatch(label,x,y,s,"depth",got.depth,expected_depth);
        if(got.stencil!=stencil) mismatch(label,x,y,s,"stencil",got.stencil,stencil);
        if(got.color!=expected_color) mismatch(label,x,y,s,"color",got.color,expected_color);
    }
    if(mode==4 || mode==5) for(uint32_t y=0;y<DP_H;++y) for(uint32_t x=0;x<DP_W;++x) {
        uint32_t got=((uint32_t*)rgba.map)[(size_t)y*DP_W+x],expected=mode==4 && y<height?dp_color(tile):0;
        if(got!=expected) mismatch(label,x,y,0,"resolve1x",got,expected);
    }
    printf("depth-probe readback=%s hash_observed=%016llX hash_expected=%016llX first_depth=%08X first_stencil=%02X first_color=%08X\n",label,(unsigned long long)observed,(unsigned long long)expected_hash,data[0].depth,data[0].stencil,data[0].color);
    if(mode==4) memcpy(composite+(size_t)tile*256*DP_W*4,rgba.map,(size_t)height*DP_W*4);
    printf("%s depth-probe checkpoint=%s checked_samples=%llu new_mismatches=%u\n",before==mismatches?"PASS":"FAIL",label,(unsigned long long)image->width*DP_H*image->samples,mismatches-before); fflush(stdout);
}
static void clear_tile(uint32_t height) {
    transition(&dest,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,STAGES_DS,ACCESS_DS);
    transition(&color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    transition(&resolved,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=color_pass,.framebuffer=color_fb,.renderArea={{0,0},{DP_W,DP_H}}}; rc_vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment a[2]={{.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,.clearValue.depthStencil={0,0}},{.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=0,.clearValue.color={{0,0,0,0}}}};
    VkClearRect rect={.rect={{0,0},{DP_W,height}},.layerCount=1}; clear_attachments(command,2,a,1,&rect); rc_vkCmdEndRenderPass(command);
}
static void initialize(void) {
    if(rcvk_load_global()) blocked("global Vulkan command missing");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="R-comp original depth transfer probe",.apiVersion=VK_API_VERSION_1_0}; VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app}; V(rc_vkCreateInstance(&ici,NULL,&instance)); if(rcvk_load_instance(instance)) blocked("instance command missing");
    image_format_properties=(PFN_vkGetPhysicalDeviceImageFormatProperties)vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceImageFormatProperties"); if(!image_format_properties) blocked("vkGetPhysicalDeviceImageFormatProperties missing");
    uint32_t count=0; V(rc_vkEnumeratePhysicalDevices(instance,&count,NULL)); if(!count) blocked("no physical device"); VkPhysicalDevice *devices=calloc(count,sizeof(*devices)); if(!devices) blocked("physical device allocation failed"); V(rc_vkEnumeratePhysicalDevices(instance,&count,devices)); physical=devices[0]; free(devices);
    VkPhysicalDeviceProperties props; VkPhysicalDeviceFeatures features; rc_vkGetPhysicalDeviceProperties(physical,&props); rc_vkGetPhysicalDeviceFeatures(physical,&features); rc_vkGetPhysicalDeviceMemoryProperties(physical,&memories);
    printf("depth-probe device=%s standardSampleLocations=%u sampleRateShading=%u depthSamples=%X stencilSamples=%X colorSamples=%X\n",props.deviceName,props.limits.standardSampleLocations,features.sampleRateShading,props.limits.framebufferDepthSampleCounts,props.limits.framebufferStencilSampleCounts,props.limits.framebufferColorSampleCounts);
    if(!props.limits.standardSampleLocations) blocked("standardSampleLocations false");
    if(!features.sampleRateShading) blocked("sampleRateShading unsupported");
    if((props.limits.framebufferDepthSampleCounts&6)!=6 || (props.limits.framebufferStencilSampleCounts&6)!=6 || !(props.limits.framebufferColorSampleCounts&2)) blocked("required MSAA depth/stencil4x+2x/color2x unsupported");
    if(props.limits.maxStorageBufferRange<(VkDeviceSize)DP_W*DP_H*2*sizeof(struct Sample)) blocked("maxStorageBufferRange too small");
    uint32_t ec=0; V(rc_vkEnumerateDeviceExtensionProperties(physical,NULL,&ec,NULL)); VkExtensionProperties *extensions=calloc(ec,sizeof(*extensions)); if(!extensions) blocked("extension allocation failed"); V(rc_vkEnumerateDeviceExtensionProperties(physical,NULL,&ec,extensions)); int stencil_export=0; for(uint32_t i=0;i<ec;++i) if(!strcmp(extensions[i].extensionName,VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME)) stencil_export=1; free(extensions); if(!stencil_export) blocked("VK_EXT_shader_stencil_export unavailable"); printf("depth-probe stencilExport=1 mapping=srcX(dstX>>1),srcSample((dstX&1)|((dstSample^1)<<1))\n");
    uint32_t qc=0; rc_vkGetPhysicalDeviceQueueFamilyProperties(physical,&qc,NULL); VkQueueFamilyProperties *families=calloc(qc,sizeof(*families)); if(!families) blocked("queue allocation failed"); rc_vkGetPhysicalDeviceQueueFamilyProperties(physical,&qc,families); uint32_t qi=qc; for(uint32_t i=0;i<qc;++i) if((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) {qi=i;break;} free(families); if(qi==qc) blocked("graphics+compute queue unavailable");
    float priority=1; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qi,.queueCount=1,.pQueuePriorities=&priority}; const char *enabled=VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME; VkPhysicalDeviceFeatures requested={.sampleRateShading=VK_TRUE}; VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=1,.ppEnabledExtensionNames=&enabled,.pEnabledFeatures=&requested}; V(rc_vkCreateDevice(physical,&dci,NULL,&device)); if(rcvk_load_device(device)) blocked("device command missing"); rc_vkGetDeviceQueue(device,qi,0,&queue);
#define EXTRA(variable,name) do {variable=(PFN_##name)rc_vkGetDeviceProcAddr(device,#name);if(!variable) blocked(#name " missing");} while(0)
    EXTRA(create_compute_pipelines,vkCreateComputePipelines); EXTRA(create_sampler,vkCreateSampler); EXTRA(destroy_sampler,vkDestroySampler); EXTRA(dispatch,vkCmdDispatch); EXTRA(push_constants,vkCmdPushConstants); EXTRA(clear_attachments,vkCmdClearAttachments);
#undef EXTRA
    VkFormatProperties fp; rc_vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_D32_SFLOAT_S8_UINT,&fp); if((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))!=(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) blocked("D32_SFLOAT_S8_UINT attachment/sampled support missing");
    source=image_create(DP_SW,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_SAMPLE_COUNT_4_BIT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);
    dest=image_create(DP_W,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_SAMPLE_COUNT_2_BIT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);
    color=image_create(DP_W,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_2_BIT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    resolved=image_create(DP_W,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    samples=buffer_create((VkDeviceSize)DP_W*DP_H*2*sizeof(struct Sample),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); rgba=buffer_create((VkDeviceSize)DP_W*DP_H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkSamplerCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0}; V(create_sampler(device,&sci,NULL,&sampler));
    VkDescriptorSetLayoutBinding bindings[4]; memset(bindings,0,sizeof(bindings)); for(uint32_t i=0;i<4;++i) { bindings[i].binding=i;bindings[i].descriptorType=i==3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=4,.pBindings=bindings}; V(rc_vkCreateDescriptorSetLayout(device,&slci,NULL,&descriptor_layout)); VkDescriptorPoolSize pool_sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,6},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}}; VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,.poolSizeCount=2,.pPoolSizes=pool_sizes}; V(rc_vkCreateDescriptorPool(device,&dpci,NULL,&descriptor_pool));
    VkDescriptorSetLayout layouts[2]={descriptor_layout,descriptor_layout};VkDescriptorSet sets[2];VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=2,.pSetLayouts=layouts};V(rc_vkAllocateDescriptorSets(device,&dsai,sets));source_set=sets[0];dest_set=sets[1];write_descriptors(source_set,&source);write_descriptors(dest_set,&dest);descriptors(&source);
    VkPushConstantRange pcr={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,.size=sizeof(struct Push)}; VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&descriptor_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&pcr}; V(rc_vkCreatePipelineLayout(device,&plci,NULL,&pipeline_layout));
    source_pass=render_pass(VK_SAMPLE_COUNT_4_BIT,0);dest_pass=render_pass(VK_SAMPLE_COUNT_2_BIT,0);color_pass=render_pass(VK_SAMPLE_COUNT_2_BIT,1);source_fb=framebuffer(source_pass,&source,0);dest_fb=framebuffer(dest_pass,&dest,0);color_fb=framebuffer(color_pass,&dest,1);
    source_fill=graphics_pipeline(source_pass,VK_SAMPLE_COUNT_4_BIT,0,0);dest_fill=graphics_pipeline(dest_pass,VK_SAMPLE_COUNT_2_BIT,0,0);transfer_pipeline=graphics_pipeline(dest_pass,VK_SAMPLE_COUNT_2_BIT,1,0);opaque_pipeline=graphics_pipeline(color_pass,VK_SAMPLE_COUNT_2_BIT,0,1);
    VkShaderModule compute=shader(snapshot_comp_spv,sizeof(snapshot_comp_spv)); VkComputePipelineCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=compute,.pName="main"},.layout=pipeline_layout,.basePipelineIndex=-1}; V(create_compute_pipelines(device,VK_NULL_HANDLE,1,&cpci,NULL,&compute_pipeline));rc_vkDestroyShaderModule(device,compute,NULL);
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=qi}; V(rc_vkCreateCommandPool(device,&cpi,NULL,&command_pool)); VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}; V(rc_vkAllocateCommandBuffers(device,&cai,&command)); VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; V(rc_vkCreateFence(device,&fci,NULL,&fence));
}
static void image_destroy(struct Image *image) { if(image->stencil) rc_vkDestroyImageView(device,image->stencil,NULL);if(image->depth)rc_vkDestroyImageView(device,image->depth,NULL);rc_vkDestroyImageView(device,image->both,NULL);rc_vkDestroyImage(device,image->image,NULL);rc_vkFreeMemory(device,image->memory,NULL); }
static void buffer_destroy(struct Buffer *b) {rc_vkUnmapMemory(device,b->memory);rc_vkDestroyBuffer(device,b->buffer,NULL);rc_vkFreeMemory(device,b->memory,NULL);}
static void cleanup(void) {
    V(rc_vkDeviceWaitIdle(device));rc_vkDestroyFence(device,fence,NULL);rc_vkDestroyCommandPool(device,command_pool,NULL);
    VkPipeline pipelines[5]={source_fill,dest_fill,transfer_pipeline,opaque_pipeline,compute_pipeline};for(unsigned i=0;i<5;++i)rc_vkDestroyPipeline(device,pipelines[i],NULL);
    rc_vkDestroyFramebuffer(device,source_fb,NULL);rc_vkDestroyFramebuffer(device,dest_fb,NULL);rc_vkDestroyFramebuffer(device,color_fb,NULL);rc_vkDestroyRenderPass(device,source_pass,NULL);rc_vkDestroyRenderPass(device,dest_pass,NULL);rc_vkDestroyRenderPass(device,color_pass,NULL);
    rc_vkDestroyPipelineLayout(device,pipeline_layout,NULL);rc_vkDestroyDescriptorPool(device,descriptor_pool,NULL);rc_vkDestroyDescriptorSetLayout(device,descriptor_layout,NULL);destroy_sampler(device,sampler,NULL);buffer_destroy(&samples);buffer_destroy(&rgba);image_destroy(&source);image_destroy(&dest);image_destroy(&color);image_destroy(&resolved);rc_vkDestroyDevice(device,NULL);rc_vkDestroyInstance(instance,NULL);
}
int main(int argc,char **argv) {
    (void)argc;(void)argv;
    int aborted=setjmp(abort_point);
    /* PS5 titles return a real status to the native shell, which parks.
     * Submitted GPU errors do not clean up or release their resources. */
    if(aborted) return aborted;
#if defined(RCOMP_TARGET_PS5)
    printf("depth-probe target=PS5/R-comp-public-RADV\n");
#else
    printf("depth-probe target=HOST-ONLY; PS5 NOT TESTED\n");
#endif
    composite=malloc((size_t)DP_W*720*4);if(!composite) blocked("composite allocation failed");
    initialize();
    printf("depth-probe source=640x1024/D32S8/4x dest=1280x1024/D32S8/2x TransferAddressConstant=00001010 directFloat32=1 transferNOT_EQUAL=5 stencilALWAYS=7/passREPLACE/zfailREPLACE; opaqueGEQUAL=6/stencilALWAYS=7/passREPLACE/zfailKEEP\n");
    begin();transition(&color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);VkClearColorValue zero={{0,0,0,0}};VkImageSubresourceRange range={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1};rc_vkCmdClearColorImage(command,color.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);transition(&color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    struct Push pc=push(0,&source);draw(source_pass,source_fb,source_fill,&source,&pc,DP_H,DP_H,0);snapshot(&source,0);compare("source4x-patterns",&source,0,DP_H,0);
    begin();pc=push(1,&dest);draw(dest_pass,dest_fb,dest_fill,&dest,&pc,DP_H,DP_H,0);finish();descriptors(&source);begin();transition(&source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);pc=push(0,&dest);draw(dest_pass,dest_fb,transfer_pipeline,&dest,&pc,DP_H,256,0);snapshot(&dest,0);compare("pattern-transfer256",&dest,1,256,0);
    begin();pc=push(2,&dest);draw(dest_pass,dest_fb,dest_fill,&dest,&pc,DP_H,DP_H,0);finish();descriptors(&source);begin();transition(&source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);pc=push(0,&dest);draw(dest_pass,dest_fb,transfer_pipeline,&dest,&pc,DP_H,208,0);snapshot(&dest,0);compare("equal-depth-stencil-zfail-replace208",&dest,2,208,0);
    begin();pc=push(3,&source);pc.tag=3;draw(source_pass,source_fb,source_fill,&source,&pc,DP_H,DP_H,0);pc=push(1,&dest);draw(dest_pass,dest_fb,dest_fill,&dest,&pc,DP_H,DP_H,0);transition(&source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);finish();descriptors(&source);begin();pc=push(0,&dest);draw(dest_pass,dest_fb,transfer_pipeline,&dest,&pc,DP_H,256,0);snapshot(&dest,0);compare("constant-import-quarter",&dest,3,256,0);
    static const uint32_t heights[3]={256,256,208},viewports[3]={720,464,208},tags[3]={0x82,0x81,8};static const float scales[3]={-1.0f,-1.5517242f,-3.46153855f},offsets[3]={0.00138888892f,-0.549568951f,-2.45673084f};
    for(uint32_t tile=0;tile<3;++tile) {
        printf("depth-probe tile=%u viewport=1280x%u scissor=1280x%u ndcY=%.9g,%.9g GEQUAL depth=.25\n",tile,viewports[tile],heights[tile],(double)scales[tile],(double)offsets[tile]);
        begin();pc=push(4,&dest);pc.tag=tags[tile];pc.yscale=scales[tile];pc.yoffset=offsets[tile];pc.xoffset=0.000781250012f;draw(color_pass,color_fb,opaque_pipeline,&dest,&pc,viewports[tile],heights[tile],1);snapshot(&dest,1);char label[64];snprintf(label,sizeof(label),"opaque-tile%u-preclear",tile);compare(label,&dest,4,heights[tile],tile);
        begin();clear_tile(heights[tile]);snapshot(&dest,1);snprintf(label,sizeof(label),"clear-tile%u-depth0-stencil0",tile);compare(label,&dest,5,heights[tile],tile);
    }
    /* Original synthetic pixels collected before each clear, assembled in order. */
#if defined(RCOMP_TARGET_PS5)
    const char *ppm_path="/app0/depth_tiles.ppm";
#else
    const char *ppm_path="depth_tiles.ppm";
#endif
    FILE *ppm=fopen(ppm_path,"wb");
    if(!ppm) {fprintf(stderr,"FAIL depth-probe cannot write composite %s\n",ppm_path);++mismatches;}
    else {
        int io_failed=fprintf(ppm,"P6\n%u 720\n255\n",DP_W)<0;
        for(size_t i=0;i<(size_t)DP_W*720;++i) if(fwrite(composite+i*4,1,3,ppm)!=3) {io_failed=1;break;}
        if(fclose(ppm)) io_failed=1;
        if(io_failed) {fprintf(stderr,"FAIL depth-probe composite write\n");++mismatches;}
        else printf("depth-probe composite=%s expected=red256,green256,blue208 size=1280x720\n",ppm_path);
    }
    free(composite);
    unsigned total=mismatches;cleanup();printf("%s depth-probe total_mismatches=%u; driver cause only if isolated GPU oracle fails\n",total?"FAIL":"PASS",total);fflush(stdout);return total?1:0;
}
