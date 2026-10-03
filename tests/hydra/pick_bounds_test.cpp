#include "../../src/hydra/published_pick_scene.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* reason) {
    if(!value) throw std::runtime_error(reason);
}
}
int main() {
    try {
        using prism::hydra::GetPickRasterBounds;
        const pxr::GfMatrix4d identity(1);
        auto full=GetPickRasterBounds(identity,identity,256,128);
        require(full.xBegin==0 && full.xEnd==256 && full.yBegin==0 && full.yEnd==128,
                "Full frustum excludes raster edge centers");
        pxr::GfMatrix4d narrow(1);
        narrow[0][0]=.01; narrow[1][1]=.02;
        narrow[3][0]=.4; narrow[3][1]=-.2;
        auto bounds=GetPickRasterBounds(identity,narrow,256,256);
        require(bounds.xBegin<=179 && bounds.xEnd>179 &&
                bounds.yBegin<=102 && bounds.yEnd>102,
                "Small pick frustum excludes its center pixel");
        require(bounds.xEnd-bounds.xBegin<8 && bounds.yEnd-bounds.yBegin<10,
                "Small pick frustum scans the entire raster");
        narrow[3][0]=3;
        bounds=GetPickRasterBounds(identity,narrow,256,256);
        require(bounds.xBegin==256 && bounds.xEnd==256,
                "Offscreen pick frustum unexpectedly contains pixels");
        auto crossing=identity; crossing[0][3]=2;
        bounds=GetPickRasterBounds(crossing,identity,256,128);
        require(bounds.xBegin==0 && bounds.xEnd==256 && bounds.yBegin==0 && bounds.yEnd==128,
                "Camera-plane crossing does not use conservative full bounds");
        bounds=GetPickRasterBounds(identity,identity,0,128);
        require(bounds.xBegin==bounds.xEnd && bounds.yBegin==bounds.yEnd,
                "Empty viewport yields nonempty pick bounds");
        std::cout << "Pick bounds retain full-resolution pixel centers and bound small queries\n";
        return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
