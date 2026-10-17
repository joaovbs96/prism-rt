// Metal device resources: SM 6.6 descriptor heaps, bound by Metal shader
// converter at kIRDescriptorHeapBindPoint and kIRSamplerHeapBindPoint.

#pragma once

StructuredBuffer<GpuFrame> frameBuffer() { return ResourceDescriptorHeap[kSlotFrame]; }
StructuredBuffer<float> positionsBuffer() { return ResourceDescriptorHeap[kSlotPositions]; }
StructuredBuffer<float> normalsBuffer() { return ResourceDescriptorHeap[kSlotNormals]; }
StructuredBuffer<float> texcoordsBuffer() { return ResourceDescriptorHeap[kSlotTexcoords]; }
StructuredBuffer<float> tangentsBuffer() { return ResourceDescriptorHeap[kSlotTangents]; }
StructuredBuffer<uint> indicesBuffer() { return ResourceDescriptorHeap[kSlotIndices]; }
StructuredBuffer<GpuPrototype> prototypesBuffer() { return ResourceDescriptorHeap[kSlotPrototypes]; }
StructuredBuffer<GpuInstance> instancesBuffer() { return ResourceDescriptorHeap[kSlotInstances]; }
StructuredBuffer<uint> primitiveMaterialsBuffer() { return ResourceDescriptorHeap[kSlotPrimitiveMaterials]; }
StructuredBuffer<GpuMaterial> materialsBuffer() { return ResourceDescriptorHeap[kSlotMaterials]; }
StructuredBuffer<GpuLegacyTexture> legacyTexturesBuffer() { return ResourceDescriptorHeap[kSlotLegacyTextures]; }
StructuredBuffer<float4> spheresBuffer() { return ResourceDescriptorHeap[kSlotSpheres]; }
StructuredBuffer<GpuEnvironment> environmentBuffer() { return ResourceDescriptorHeap[kSlotEnvironment]; }
StructuredBuffer<float> envTexelPdfBuffer() { return ResourceDescriptorHeap[kSlotEnvTexelPdf]; }
StructuredBuffer<float> envAliasProbabilityBuffer() { return ResourceDescriptorHeap[kSlotEnvAliasProbability]; }
StructuredBuffer<uint> envAliasBuffer() { return ResourceDescriptorHeap[kSlotEnvAlias]; }
StructuredBuffer<uint> argumentBlocksBuffer() { return ResourceDescriptorHeap[kSlotArgumentBlocks]; }
StructuredBuffer<uint> roDataBuffer() { return ResourceDescriptorHeap[kSlotRoData]; }
StructuredBuffer<GpuMdlTexture> mdlTexturesBuffer() { return ResourceDescriptorHeap[kSlotMdlTextures]; }
StructuredBuffer<uint> mdlTilesBuffer() { return ResourceDescriptorHeap[kSlotMdlTiles]; }
StructuredBuffer<float> bsdfDataBuffer() { return ResourceDescriptorHeap[kSlotBsdfData]; }
StructuredBuffer<GpuImage> imagesBuffer() { return ResourceDescriptorHeap[kSlotImages]; }
StructuredBuffer<GpuTile> tilesBuffer() { return ResourceDescriptorHeap[kSlotTiles]; }
RWStructuredBuffer<uint> rngBuffer() { return ResourceDescriptorHeap[kSlotRng]; }
RWStructuredBuffer<float4> outputBuffer() { return ResourceDescriptorHeap[kSlotOutput]; }
RWStructuredBuffer<float4> accumulationBuffer() { return ResourceDescriptorHeap[kSlotAccumulation]; }
RaytracingAccelerationStructure sceneStructure() { return ResourceDescriptorHeap[kSlotAccel]; }
Texture2D<float4> imageTexture(uint image) { return ResourceDescriptorHeap[NonUniformResourceIndex(kSlotImageBase + image)]; }
SamplerState imageSampler(uint index) { return SamplerDescriptorHeap[index]; }
