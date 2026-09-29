#include "../../src/hydra/render_buffer.h"
#include "../../src/hydra/scene_bridge.h"

#include <pxr/base/gf/vec3i.h>
#include <future>
#include <chrono>
#include <iostream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

int main() {
    try {
        PrismRenderBuffer buffer(SdfPath("/color"), nullptr);
        auto frame=std::make_shared<prism::hydra::PublishedFrame>();
        frame->generation=7;
        if (!buffer.Allocate(GfVec3i(2, 2, 1), HdFormatFloat32, false)) return 1;
        buffer.Clear(VtValue(1.0f));
        const auto* first = static_cast<const float*>(buffer.Map());
        auto publish = std::async(std::launch::async, [&] {
            const float value = 2;
            for (unsigned y=0;y<2;++y) for (unsigned x=0;x<2;++x)
                buffer.WriteFloat(x,y,&value,1,false);
            buffer.Publish(frame);
        });
        if (publish.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            buffer.Unmap();
            throw std::runtime_error("publication blocked on a mapped image");
        }
        publish.get();
        if (first[0]!=1 || first[3]!=1) throw std::runtime_error("mapped generation was overwritten");
        if(buffer.Capture().frame) throw std::runtime_error("mapped metadata changed during publication");
        // Nested mappings pin the same generation until all users release it.
        if (buffer.Map()!=first) throw std::runtime_error("nested mapping changed generation");
        buffer.Unmap();
        buffer.Unmap();
        auto* second = static_cast<const float*>(buffer.Map());
        const auto retained=buffer.Capture();
        if(retained.frame!=frame || retained.width!=2 || retained.height!=2)
            throw std::runtime_error("published metadata does not match pixels");
        if(second[0]!=2 || second[3]!=2) throw std::runtime_error("publication missing");
        buffer.Allocate(GfVec3i(3,1,1),HdFormatFloat32,false);
        if(buffer.Capture().width!=2 || buffer.Capture().frame!=frame)
            throw std::runtime_error("resize changed mapped metadata");
        if(second[3]!=2) throw std::runtime_error("resize invalidated mapping");
        buffer.Unmap();
        if(buffer.Capture().width!=3 || buffer.Capture().frame)
            throw std::runtime_error("allocation retained stale metadata");
        if(retained.frame->generation!=7 || reinterpret_cast<float const*>(retained.pixels->data())[3]!=2)
            throw std::runtime_error("captured view did not retain frame lifetime");
        buffer.PublishPreviewFloat(1,1,{3.0f},1);
        auto* preview=static_cast<const float*>(buffer.Map());
        if(preview[0]!=3 || preview[2]!=3) throw std::runtime_error("preview expansion failed");
        buffer.Unmap();
        std::cout << "Mapped storage survives publication, nested maps, and resize\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
