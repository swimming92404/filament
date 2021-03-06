/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gltfio/AssetPipeline.h>

#include <xatlas.h>

#define CGLTF_WRITE_IMPLEMENTATION
#include "cgltf_write.h"

#include <utils/Log.h>

#include <math/mat4.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <memory>
#include <vector>

using namespace filament::math;
using namespace utils;

using std::vector;

namespace {

using AssetHandle = gltfio::AssetPipeline::AssetHandle;

static const char* const POSITION = "POSITION";
static const char* const NORMAL = "NORMAL";
static const char* const TANGENT = "TANGENT";
static const char* const GENERATOR_ID = "gltfio";

// Bookkeeping structure for baking a single primitive + node pair.
struct BakedPrim {
    const cgltf_node* sourceNode;
    const cgltf_mesh* sourceMesh;
    const cgltf_primitive* sourcePrimitive;
    const cgltf_attribute* sourcePositions;
    const cgltf_attribute* sourceNormals;
    const cgltf_attribute* sourceTangents;
    float3* bakedPositions;
    float3* bakedNormals;
    float4* bakedTangents;
    uint32_t* bakedIndices;
    float3 bakedMin;
    float3 bakedMax;
};

// Utility class to help populate cgltf arrays and ensure that the memory is freed.
template <typename T>
class ArrayHolder {
    vector<std::unique_ptr<T[]> > mItems;
public:
    T* alloc(size_t count) {
        mItems.push_back(std::make_unique<T[]>(count));
        return mItems.back().get();
    }
};

// Private implementation for AssetPipeline.
class Pipeline {
public:
    // Aggregate all buffers into a single buffer.
    const cgltf_data* flattenBuffers(const cgltf_data* sourceAsset);

    // Bake transforms and make each primitive correspond to a single node.
    const cgltf_data* flattenPrims(const cgltf_data* sourceAsset, uint32_t flags);

    // Use xatlas to generate a new UV set and modify topology appropriately.
    const cgltf_data* parameterize(const cgltf_data* sourceAsset);

    // Take ownership of the given asset and free it when the pipeline is destroyed.
    void addSourceAsset(cgltf_data* asset);

    ~Pipeline();

private:
    void bakeTransform(BakedPrim* prim, const mat4f& transform, const mat3f& normalMatrix);
    void populateResult(const BakedPrim* prims, size_t numPrims, size_t numVerts);
    bool filterPrim(const cgltf_primitive& prim);

    uint32_t mFlattenFlags;
    vector<cgltf_data*> mSourceAssets;

    struct {
        ArrayHolder<cgltf_data> resultAssets;
        ArrayHolder<cgltf_scene> scenes;
        ArrayHolder<cgltf_node*> nodePointers;
        ArrayHolder<cgltf_node> nodes;
        ArrayHolder<cgltf_mesh> meshes;
        ArrayHolder<cgltf_primitive> prims;
        ArrayHolder<cgltf_attribute> attributes;
        ArrayHolder<cgltf_accessor> accessors;
        ArrayHolder<cgltf_buffer_view> views;
        ArrayHolder<cgltf_buffer> buffers;
        ArrayHolder<cgltf_image> images;
        ArrayHolder<cgltf_texture> textures;
        ArrayHolder<cgltf_material> materials;
        ArrayHolder<uint8_t> bufferData;
    } mStorage;
};

// Returns true if the given cgltf asset has been flattened by the mesh pipeline and is therefore
// amenable to subsequent pipeline operations like baking and exporting.
bool isFlattened(const cgltf_data* asset) {
    return asset && asset->buffers_count == 1 && asset->nodes_count == asset->meshes_count &&
            asset->asset.generator == GENERATOR_ID;
}

// Returns true if the given primitive should be baked out, false if it should be culled away.
bool Pipeline::filterPrim(const cgltf_primitive& prim) {
    const bool filterTriangles = mFlattenFlags & gltfio::AssetPipeline::FILTER_TRIANGLES;
    if (filterTriangles && prim.type != cgltf_primitive_type_triangles) {
        return false;
    }
    for (cgltf_size k = 0; k < prim.attributes_count; ++k) {
        const cgltf_attribute& attr = prim.attributes[k];
        if (!attr.data || !attr.data->count || attr.data->is_sparse) {
            return false;
        }
    }
    if (!prim.indices || prim.indices->is_sparse) {
        // TODO: generate trivial indices
        return false;
    }
    return true;
}

const cgltf_data* Pipeline::flattenBuffers(const cgltf_data* sourceAsset) {
    // Determine the total required size for the aggregated buffer.
    size_t totalSize = 0;
    for (size_t i = 0, len = sourceAsset->buffers_count; i < len; ++i) {
        totalSize += sourceAsset->buffers[i].size;
    }

    // Count the total number of attributes and primitives.
    size_t attribsCount = 0;
    size_t primsCount = 0;
    for (size_t i = 0, len = sourceAsset->meshes_count; i < len; ++i) {
        const auto& mesh = sourceAsset->meshes[i];
        primsCount += mesh.primitives_count;
        for (size_t j = 0; j < mesh.primitives_count; ++j) {
            attribsCount += mesh.primitives[j].attributes_count;
        }
    }

    // Count the total number of referenced nodes.
    size_t nodePointersCount = 0;
    for (size_t i = 0, len = sourceAsset->scenes_count; i < len; ++i) {
        const auto& scene = sourceAsset->scenes[i];
        nodePointersCount += scene.nodes_count;
    }

    // Allocate inner lists.
    uint8_t* bufferData = mStorage.bufferData.alloc(totalSize);
    cgltf_primitive* primitives = mStorage.prims.alloc(primsCount);
    cgltf_attribute* attributes = mStorage.attributes.alloc(attribsCount);
    cgltf_node** nodePointers = mStorage.nodePointers.alloc(nodePointersCount);

    // Allocate top-level structs.
    cgltf_buffer* buffer = mStorage.buffers.alloc(1);
    cgltf_buffer_view* views = mStorage.views.alloc(sourceAsset->buffer_views_count);
    cgltf_accessor* accessors = mStorage.accessors.alloc(sourceAsset->accessors_count);
    cgltf_image* images = mStorage.images.alloc(sourceAsset->images_count);
    cgltf_texture* textures = mStorage.textures.alloc(sourceAsset->textures_count);
    cgltf_material* materials = mStorage.materials.alloc(sourceAsset->materials_count);
    cgltf_mesh* meshes = mStorage.meshes.alloc(sourceAsset->meshes_count);
    cgltf_node* nodes = mStorage.nodes.alloc(sourceAsset->nodes_count);
    cgltf_scene* scenes = mStorage.scenes.alloc(sourceAsset->scenes_count);
    cgltf_data* resultAsset = mStorage.resultAssets.alloc(1);

    // Populate the new buffer object.
    size_t offset = 0;
    vector<size_t> offsets(sourceAsset->buffers_count);
    for (size_t i = 0, len = sourceAsset->buffers_count; i < len; ++i) {
        size_t size = sourceAsset->buffers[i].size;
        memcpy(bufferData + offset, sourceAsset->buffers[i].data, size);
        offsets[i] = offset;
        offset += size;
    }
    buffer->size = totalSize;
    buffer->data = bufferData;

    // Populate the buffer views.
    for (size_t i = 0, len = sourceAsset->buffer_views_count; i < len; ++i) {
        auto& view = views[i] = sourceAsset->buffer_views[i];
        size_t bufferIndex = view.buffer - sourceAsset->buffers;
        view.buffer = buffer;
        view.offset += offsets[bufferIndex];
    }

    // Clone the accessors.
    for (size_t i = 0, len = sourceAsset->accessors_count; i < len; ++i) {
        auto& accessor = accessors[i] = sourceAsset->accessors[i];
        accessor.buffer_view = views + (accessor.buffer_view - sourceAsset->buffer_views);
    }

    // Clone the images.
    for (size_t i = 0, len = sourceAsset->images_count; i < len; ++i) {
        auto& image = images[i] = sourceAsset->images[i];
        if (image.buffer_view) {
            image.buffer_view = views + (image.buffer_view - sourceAsset->buffer_views);
        }
    }

    // Clone the textures.
    for (size_t i = 0, len = sourceAsset->textures_count; i < len; ++i) {
        auto& texture = textures[i] = sourceAsset->textures[i];
        texture.image = images + (texture.image - sourceAsset->images);
    }

    // Clone the nodes.
    for (size_t i = 0, len = sourceAsset->nodes_count; i < len; ++i) {
        auto& node = nodes[i] = sourceAsset->nodes[i];
        if (node.mesh) {
            node.mesh = meshes + (node.mesh - sourceAsset->meshes);
        }
    }

    // Clone the scenes.
    for (size_t i = 0, len = sourceAsset->scenes_count; i < len; ++i) {
        const auto& sourceScene = sourceAsset->scenes[i];
        auto& resultScene = scenes[i] = sourceScene;
        resultScene.nodes = nodePointers;
        for (size_t j = 0; j < sourceScene.nodes_count; ++j) {
            resultScene.nodes[j] = nodes + (sourceScene.nodes[j] - sourceAsset->nodes);
        }
        nodePointers += sourceScene.nodes_count;
    }

    // Clone the materials.
    for (size_t i = 0, len = sourceAsset->materials_count; i < len; ++i) {
        auto& material = materials[i] = sourceAsset->materials[i];
        auto& t0 = material.pbr_metallic_roughness.base_color_texture.texture;
        auto& t1 = material.pbr_metallic_roughness.metallic_roughness_texture.texture;
        auto& t2 = material.pbr_specular_glossiness.diffuse_texture.texture;
        auto& t3 = material.pbr_specular_glossiness.specular_glossiness_texture.texture;
        auto& t4 = material.normal_texture.texture;
        auto& t5 = material.occlusion_texture.texture;
        auto& t6 = material.emissive_texture.texture;
        t0 = t0 ? textures + (t0 - sourceAsset->textures) : nullptr;
        t1 = t1 ? textures + (t1 - sourceAsset->textures) : nullptr;
        t2 = t2 ? textures + (t2 - sourceAsset->textures) : nullptr;
        t3 = t3 ? textures + (t3 - sourceAsset->textures) : nullptr;
        t4 = t4 ? textures + (t4 - sourceAsset->textures) : nullptr;
        t5 = t5 ? textures + (t5 - sourceAsset->textures) : nullptr;
        t6 = t6 ? textures + (t6 - sourceAsset->textures) : nullptr;
    }

    // Clone the meshes, primitives, and attributes.
    for (size_t i = 0, len = sourceAsset->meshes_count; i < len; ++i) {
        const auto& sourceMesh = sourceAsset->meshes[i];
        auto& resultMesh = meshes[i] = sourceMesh;
        resultMesh.primitives = primitives;
        for (size_t j = 0; j < sourceMesh.primitives_count; ++j) {
            const auto& sourcePrim = sourceMesh.primitives[j];
            auto& resultPrim = resultMesh.primitives[j] = sourcePrim;
            resultPrim.material = materials + (sourcePrim.material - sourceAsset->materials);
            resultPrim.attributes = attributes;
            resultPrim.indices = accessors + (sourcePrim.indices - sourceAsset->accessors);
            for (size_t k = 0; k < sourcePrim.attributes_count; ++k) {
                const auto& sourceAttr = sourcePrim.attributes[k];
                auto& resultAttr = resultPrim.attributes[k] = sourceAttr;
                resultAttr.data = accessors + (sourceAttr.data - sourceAsset->accessors);
            }
            attributes += sourcePrim.attributes_count;
        }
        primitives += sourceMesh.primitives_count;
    }

    // Clone the high-level asset structure, then substitute some of the top-level lists.
    *resultAsset = *sourceAsset;
    resultAsset->buffers = buffer;
    resultAsset->buffers_count = 1;
    resultAsset->buffer_views = views;
    resultAsset->accessors = accessors;
    resultAsset->images = images;
    resultAsset->textures = textures;
    resultAsset->materials = materials;
    resultAsset->meshes = meshes;
    resultAsset->nodes = nodes;
    resultAsset->scenes = scenes;
    resultAsset->scene = scenes + (sourceAsset->scene - sourceAsset->scenes);
    return resultAsset;
}

const cgltf_data* Pipeline::flattenPrims(const cgltf_data* sourceAsset, uint32_t flags) {
    mFlattenFlags = flags;

    // This must be called after flattenBuffers.
    assert(sourceAsset->buffers_count == 1);
    assert(sourceAsset->buffers[0].data != nullptr);

    // Determine the number of primitives and attributes that will be baked.
    size_t numPrims = 0;
    size_t numAttributes = 0;
    for (cgltf_size i = 0; i < sourceAsset->nodes_count; ++i) {
        const cgltf_node& node = sourceAsset->nodes[i];
        if (node.mesh) {
            for (cgltf_size j = 0; j < node.mesh->primitives_count; ++j) {
                cgltf_primitive& sourcePrim = node.mesh->primitives[j];
                if (filterPrim(sourcePrim)) {
                    numPrims++;
                    numAttributes += sourcePrim.attributes_count;
                }
            }
        }
    }
    vector<BakedPrim> bakedPrims;
    bakedPrims.reserve(numPrims);

    // Count the total number of vertices and start filling in the BakedPrim structs.
    int numPositions = 0, numNormals = 0, numTangents = 0, numIndices = 0;
    int numPrimsWithNormals = 0, numPrimsWithTangents = 0;
    for (cgltf_size i = 0; i < sourceAsset->nodes_count; ++i) {
        const cgltf_node& node = sourceAsset->nodes[i];
        if (node.mesh) {
            for (cgltf_size j = 0; j < node.mesh->primitives_count; ++j) {
                const cgltf_primitive& sourcePrim = node.mesh->primitives[j];
                if (filterPrim(sourcePrim)) {
                    BakedPrim bakedPrim {
                        .sourceNode = &node,
                        .sourceMesh = node.mesh,
                        .sourcePrimitive = &sourcePrim,
                    };
                    for (cgltf_size k = 0; k < sourcePrim.attributes_count; ++k) {
                        const cgltf_attribute& attr = sourcePrim.attributes[k];
                        switch (attr.type) {
                            case cgltf_attribute_type_position:
                                numPositions += attr.data->count;
                                bakedPrim.sourcePositions = &attr;
                                break;
                            case cgltf_attribute_type_tangent:
                                numPrimsWithTangents++;
                                numTangents += attr.data->count;
                                bakedPrim.sourceTangents = &attr;
                                break;
                            case cgltf_attribute_type_normal:
                                numPrimsWithNormals++;
                                numNormals += attr.data->count;
                                bakedPrim.sourceNormals = &attr;
                                break;
                            default:
                                break;
                        }
                    }
                    numIndices += sourcePrim.indices->count;
                    bakedPrims.push_back(bakedPrim);
                }
            }
        }
    }

    // Allocate a buffer large enough to hold vertex positions and indices.
    const size_t positionsDataSize = sizeof(float3) * numPositions;
    const size_t normalsDataSize = sizeof(float3) * numNormals;
    const size_t tangentsDataSize = sizeof(float4) * numTangents;
    const size_t vertexDataSize = positionsDataSize + normalsDataSize + tangentsDataSize;
    const size_t indexDataSize = sizeof(uint32_t) * numIndices;
    uint8_t* bufferData = mStorage.bufferData.alloc(vertexDataSize + indexDataSize);
    float3* bakedPositions = (float3*) bufferData;
    float3* bakedNormals = (float3*) (bufferData + positionsDataSize);
    float4* bakedTangents = (float4*) (bufferData + positionsDataSize + normalsDataSize);
    uint32_t* bakedIndices = (uint32_t*) (bufferData + vertexDataSize);

    // Next, perform the actual baking: convert all vertex positions to fp32, transform them by
    // their respective node matrix, etc.
    const cgltf_node* node = nullptr;
    mat4f matrix;
    mat3f normalMatrix;
    for (size_t i = 0; i < numPrims; ++i) {
        BakedPrim& bakedPrim = bakedPrims[i];
        if (bakedPrim.sourceNode != node) {
            node = bakedPrim.sourceNode;
            cgltf_node_transform_world(node, &matrix[0][0]);
            normalMatrix = transpose(inverse(matrix.upperLeft()));
        }
        const cgltf_primitive* sourcePrim = bakedPrim.sourcePrimitive;

        bakedPrim.bakedPositions = bakedPositions;
        bakedPositions += bakedPrim.sourcePositions->data->count;

        bakedPrim.bakedIndices = bakedIndices;
        bakedIndices += sourcePrim->indices->count;

        if (bakedPrim.sourceNormals) {
            bakedPrim.bakedNormals = bakedNormals;
            bakedNormals += bakedPrim.sourceNormals->data->count;
        }

        if (bakedPrim.sourceTangents) {
            bakedPrim.bakedTangents = bakedTangents;
            bakedTangents += bakedPrim.sourceTangents->data->count;
        }

        bakeTransform(&bakedPrim, matrix, normalMatrix);
    }

    // Keep all buffer views + accessors from the source asset (they can be culled later) and add
    // new buffer views + accessors for indices and baked attributes.
    size_t numAttributesBaked = numPrims + numPrimsWithNormals + numPrimsWithTangents;
    size_t numBufferViews = sourceAsset->buffer_views_count + numPrims + numAttributesBaked;
    size_t numAccessors = sourceAsset->accessors_count + numPrims + numAttributesBaked;

    // Allocate memory for the various cgltf structures.
    cgltf_data* resultAsset = mStorage.resultAssets.alloc(1);
    cgltf_scene* scene = mStorage.scenes.alloc(1);
    cgltf_node** nodePointers = mStorage.nodePointers.alloc(numPrims);
    cgltf_node* nodes = mStorage.nodes.alloc(numPrims);
    cgltf_mesh* meshes = mStorage.meshes.alloc(numPrims);
    cgltf_primitive* prims = mStorage.prims.alloc(numPrims);
    cgltf_buffer_view* views = mStorage.views.alloc(numBufferViews);
    cgltf_accessor* accessors = mStorage.accessors.alloc(numAccessors);
    cgltf_attribute* attributes = mStorage.attributes.alloc(numAttributes);
    cgltf_buffer* buffers = mStorage.buffers.alloc(2);
    cgltf_image* images = mStorage.images.alloc(sourceAsset->images_count);

    // Initialize iterators for various cgtlf lists.
    cgltf_buffer_view* indicesViews = views;
    cgltf_buffer_view* positionsViews = indicesViews + numPrims;
    cgltf_buffer_view* normalsViews = positionsViews + numPrims;
    cgltf_buffer_view* tangentsViews = normalsViews + numPrimsWithNormals;
    cgltf_accessor* indicesAccessors = accessors;
    cgltf_accessor* positionsAccessors = indicesAccessors + numPrims;
    cgltf_accessor* normalsAccessors = positionsAccessors + numPrims;
    cgltf_accessor* tangentsAccessors = normalsAccessors + numPrimsWithNormals;
    cgltf_size positionsOffset = 0;
    cgltf_size normalsOffset = positionsDataSize;
    cgltf_size tangentsOffset = positionsDataSize + normalsDataSize;
    cgltf_size indicesOffset = vertexDataSize;

    // Populate the fields of the cgltf structures.
    for (size_t primIndex = 0, attrIndex = 0; primIndex < numPrims; ++primIndex) {
        BakedPrim& bakedPrim = bakedPrims[primIndex];

        nodePointers[primIndex] = nodes + primIndex;

        nodes[primIndex] = {
            .name = bakedPrim.sourceNode->name,
            .mesh = meshes + primIndex,
        };

        meshes[primIndex] = {
            .name = bakedPrim.sourceMesh->name,
            .primitives = prims + primIndex,
            .primitives_count = 1,
        };

        cgltf_accessor& indicesAccessor = *indicesAccessors++ = {
            .component_type = cgltf_component_type_r_32u,
            .type = cgltf_type_scalar,
            .count = bakedPrim.sourcePrimitive->indices->count,
            .buffer_view = indicesViews,
        };
        cgltf_buffer_view& indicesBufferView = *indicesViews++ = {
            .buffer = buffers,
            .offset = indicesOffset,
            .size = indicesAccessor.count * sizeof(uint32_t)
        };
        indicesOffset += indicesBufferView.size;

        cgltf_accessor& positionsAccessor = *positionsAccessors++ = {
            .component_type = cgltf_component_type_r_32f,
            .type = cgltf_type_vec3,
            .count = bakedPrim.sourcePositions->data->count,
            .buffer_view = positionsViews,
            .has_min = true,
            .has_max = true,
        };
        cgltf_buffer_view& positionsBufferView = *positionsViews++ = {
            .buffer = buffers,
            .offset = positionsOffset,
            .size = positionsAccessor.count * sizeof(float3)
        };
        positionsOffset += positionsBufferView.size;
        *((float3*) positionsAccessor.min) = bakedPrim.bakedMin;
        *((float3*) positionsAccessor.max) = bakedPrim.bakedMax;
        cgltf_attribute& positionsAttribute = attributes[attrIndex++] = {
            .name = (char*) POSITION,
            .type = cgltf_attribute_type_position,
            .data = &positionsAccessor
        };

        if (bakedPrim.bakedNormals) {
            cgltf_accessor& normalsAccessor = *normalsAccessors++ = {
                .component_type = cgltf_component_type_r_32f,
                .type = cgltf_type_vec3,
                .count = bakedPrim.sourceNormals->data->count,
                .buffer_view = normalsViews
            };
            cgltf_buffer_view& normalsBufferView = *normalsViews++ = {
                .buffer = buffers,
                .offset = normalsOffset,
                .size = normalsAccessor.count * sizeof(float3)
            };
            normalsOffset += normalsBufferView.size;
            attributes[attrIndex++] = {
                .name = (char*) NORMAL,
                .type = cgltf_attribute_type_normal,
                .data = &normalsAccessor
            };
        }

        if (bakedPrim.bakedTangents) {
            cgltf_accessor& tangentsAccessor = *tangentsAccessors++ = {
                .component_type = cgltf_component_type_r_32f,
                .type = cgltf_type_vec4,
                .count = bakedPrim.sourceTangents->data->count,
                .buffer_view = tangentsViews
            };
            cgltf_buffer_view& tangentsBufferView = *tangentsViews++ = {
                .buffer = buffers,
                .offset = tangentsOffset,
                .size = tangentsAccessor.count * sizeof(float4)
            };
            tangentsOffset += tangentsBufferView.size;
            attributes[attrIndex++] = {
                .name = (char*) TANGENT,
                .type = cgltf_attribute_type_tangent,
                .data = &tangentsAccessor
            };
        }

        size_t attrCount = bakedPrim.sourcePrimitive->attributes_count;
        for (size_t j = 0; j < attrCount; ++j) {
            const auto& srcAttr = bakedPrim.sourcePrimitive->attributes[j];
            if (srcAttr.type != cgltf_attribute_type_position &&
                    srcAttr.type != cgltf_attribute_type_normal &&
                    srcAttr.type != cgltf_attribute_type_tangent) {
                auto& attr = attributes[attrIndex++] = srcAttr;
                size_t accessorIndex = attr.data - sourceAsset->accessors;
                attr.data = accessors + numPrims + numAttributesBaked + accessorIndex;
            }
        }

        prims[primIndex] = {
            .type = cgltf_primitive_type_triangles,
            .indices = &indicesAccessor,
            .material = bakedPrim.sourcePrimitive->material,
            .attributes = &positionsAttribute,
            .attributes_count = attrCount,
        };
    }

    scene->name = sourceAsset->scene->name;
    scene->nodes = nodePointers;
    scene->nodes_count = numPrims;

    buffers[0].size = vertexDataSize + indexDataSize;
    buffers[0].data = bufferData;

    buffers[1] = sourceAsset->buffers[0];

    resultAsset->file_type = sourceAsset->file_type;
    resultAsset->file_data = sourceAsset->file_data;
    resultAsset->asset = sourceAsset->asset;
    resultAsset->asset.generator = (char*) GENERATOR_ID;
    resultAsset->meshes = meshes;
    resultAsset->meshes_count = numPrims;
    resultAsset->accessors = accessors;
    resultAsset->accessors_count = numAccessors;
    resultAsset->buffer_views = views;
    resultAsset->buffer_views_count = numBufferViews;
    resultAsset->buffers = buffers;
    resultAsset->buffers_count = 2;
    resultAsset->nodes = nodes;
    resultAsset->nodes_count = numPrims;
    resultAsset->scenes = scene;
    resultAsset->scenes_count = 1;
    resultAsset->scene = scene;
    resultAsset->images = images;
    resultAsset->images_count = sourceAsset->images_count;
    resultAsset->textures = sourceAsset->textures;
    resultAsset->textures_count = sourceAsset->textures_count;
    resultAsset->materials = sourceAsset->materials;
    resultAsset->materials_count = sourceAsset->materials_count;
    resultAsset->samplers = sourceAsset->samplers;
    resultAsset->samplers_count = sourceAsset->samplers_count;

    // Copy over the buffer views, accessors, and textures, then fix up the pointers.
    const size_t offset = numPrims + numAttributesBaked;
    for (size_t i = offset; i < numBufferViews; ++i) {
        auto& view = views[i] = sourceAsset->buffer_views[i - offset];
        view.buffer = &buffers[1];
    }
    for (size_t i = offset; i < numAccessors; ++i) {
        auto& accessor = accessors[i] = sourceAsset->accessors[i - offset];
        size_t viewIndex = accessor.buffer_view - sourceAsset->buffer_views;
        accessor.buffer_view = views + offset + viewIndex;
    }

    for (size_t i = 0; i < sourceAsset->images_count; ++i) {
        auto& image = images[i] = sourceAsset->images[i];
        if (image.buffer_view) {
            size_t viewIndex = image.buffer_view - sourceAsset->buffer_views;
            image.buffer_view = views + offset + viewIndex;
        }
    }
    for (size_t i = 0; i < resultAsset->textures_count; ++i) {
        size_t imageIndex = resultAsset->textures[i].image - sourceAsset->images;
        resultAsset->textures[i].image = images + imageIndex;
    }

    return resultAsset;
}

void Pipeline::bakeTransform(BakedPrim* prim, const mat4f& transform, const mat3f& normalMatrix) {
    const cgltf_primitive* source = prim->sourcePrimitive;
    const cgltf_attribute* sourcePositions = prim->sourcePositions;
    const size_t numPositions = sourcePositions->data->count;

    // Read position data, converting to float if necessary.
    cgltf_float* writePtr = &prim->bakedPositions->x;
    for (cgltf_size index = 0; index < numPositions; ++index, writePtr += 3) {
        cgltf_accessor_read_float(sourcePositions->data, index, writePtr, 3);
    }

    // Prepare for computing the post-transformed bounding box.
    float3& minpt = prim->bakedMin = std::numeric_limits<float>::max();
    float3& maxpt = prim->bakedMax = std::numeric_limits<float>::lowest();

    // Transform the positions and compute the new bounding box.
    float3* bakedPositions = prim->bakedPositions;
    for (cgltf_size index = 0; index < numPositions; ++index) {
        float3& pt = bakedPositions[index];
        pt = (transform * float4(pt, 1.0f)).xyz;
        minpt = min(minpt, pt);
        maxpt = max(maxpt, pt);
    }

    // Read index data, converting to uint32 if necessary.
    uint32_t* bakedIndices = prim->bakedIndices;
    for (cgltf_size index = 0, len = source->indices->count; index < len; ++index) {
        bakedIndices[index] = cgltf_accessor_read_index(source->indices, index);
    }

    // Transform normals if available.
    if (prim->bakedNormals) {
        const cgltf_attribute* sourceNormals = prim->sourceNormals;
        const size_t numNormals = sourceNormals->data->count;
        cgltf_float* writePtr = &prim->bakedNormals->x;
        for (cgltf_size index = 0; index < numNormals; ++index, writePtr += 3) {
            cgltf_accessor_read_float(sourceNormals->data, index, writePtr, 3);
        }
        float3* bakedNormals = prim->bakedNormals;
        for (cgltf_size index = 0; index < numNormals; ++index) {
            float3& n = bakedNormals[index];
            n = normalMatrix * n;
        }
    }

    // Transform tangents if available.
    if (prim->bakedTangents) {
        const cgltf_attribute* sourceTangents = prim->sourceTangents;
        const size_t numTangents = sourceTangents->data->count;
        cgltf_float* writePtr = &prim->bakedTangents->x;
        for (cgltf_size index = 0; index < numTangents; ++index, writePtr += 4) {
            cgltf_accessor_read_float(sourceTangents->data, index, writePtr, 4);
        }
        float4* bakedTangents = prim->bakedTangents;
        for (cgltf_size index = 0; index < numTangents; ++index) {
            float3& t = bakedTangents[index].xyz;
            t = normalMatrix * t;
        }
    }
}

const cgltf_data* Pipeline::parameterize(const cgltf_data* sourceAsset) {
    auto atlas = xatlas::Create();

    utils::slog.e << "parameterize is not yet implemented." << utils::io::endl;

    xatlas::Destroy(atlas);
    atlas = nullptr;
    return nullptr;
}

void Pipeline::addSourceAsset(cgltf_data* asset) {
    mSourceAssets.push_back(asset);
}

Pipeline::~Pipeline() {
    for (auto asset : mSourceAssets) {
        cgltf_free(asset);
    }
}

} // anonymous namespace

namespace gltfio {

AssetPipeline::AssetPipeline() {
    mImpl = new Pipeline();
}

AssetPipeline::~AssetPipeline() {
    Pipeline* impl = (Pipeline*) mImpl;
    delete impl;
}

AssetHandle AssetPipeline::flatten(AssetHandle source, uint32_t flags) {
    Pipeline* impl = (Pipeline*) mImpl;
    const cgltf_data* asset = (const cgltf_data*) source;
    if (asset->buffers_count > 1) {
        asset = impl->flattenBuffers(asset);
    }
    asset = impl->flattenPrims(asset, flags);
    asset = impl->flattenBuffers(asset);
    return asset;
}

AssetHandle AssetPipeline::load(const utils::Path& fileOrDirectory) {
    utils::Path filename = fileOrDirectory;
    if (!filename.exists()) {
        utils::slog.e << "file " << filename << " not found!" << utils::io::endl;
        return nullptr;
    }
    if (filename.isDirectory()) {
        auto files = filename.listContents();
        for (auto file : files) {
            if (file.getExtension() == "gltf") {
                filename = file;
                break;
            }
        }
        if (filename.isDirectory()) {
            utils::slog.e << "no glTF file found in " << filename << utils::io::endl;
            return nullptr;
        }
    }

    // Parse the glTF file.
    cgltf_options options { cgltf_file_type_gltf };
    cgltf_data* sourceAsset;
    cgltf_result result = cgltf_parse_file(&options, filename.c_str(), &sourceAsset);
    if (result != cgltf_result_success) {
        return nullptr;
    }
    Pipeline* impl = (Pipeline*) mImpl;
    impl->addSourceAsset(sourceAsset);

    // Load external resources.
    utils::Path abspath = filename.getAbsolutePath();
    if (cgltf_load_buffers(&options, sourceAsset, abspath.c_str()) != cgltf_result_success) {
        utils::slog.e << "Unable to load external buffers." << utils::io::endl;
        exit(1);
    }

    return sourceAsset;
}

void AssetPipeline::save(AssetHandle handle, const utils::Path& jsonPath,
        const utils::Path& binPath) {
    cgltf_data* asset = (cgltf_data*) handle;

    if (!isFlattened(asset)) {
        utils::slog.e << "Only flattened assets can be exported to disk." << utils::io::endl;
        return;
    }

    std::string binName = binPath.getName();
    asset->buffers[0].uri = (char*) (binName.c_str());
    cgltf_options options { cgltf_file_type_gltf };
    cgltf_write_file(&options, jsonPath.c_str(), asset);
    asset->buffers[0].uri = nullptr;

    FILE* binFile = fopen(binPath.c_str(), "wb");
    fwrite((char*) asset->buffers[0].data, asset->buffers[0].size, 1, binFile);
    fclose(binFile);
}

AssetHandle AssetPipeline::parameterize(AssetHandle source) {
    Pipeline* impl = (Pipeline*) mImpl;
    return impl->parameterize((const cgltf_data*) source);
}

}  // namespace gltfio
