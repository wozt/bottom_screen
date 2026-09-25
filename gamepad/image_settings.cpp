#include "image_settings.h"
#include <algorithm>
extern "C" {
#include <libswscale/swscale.h>
}
int ImageScaleFlags(int filter)
{
    const int flags[]={SWS_BILINEAR,SWS_BICUBIC,SWS_LANCZOS,SWS_POINT};
    return flags[std::clamp(filter,0,3)];
}
void ProcessImage(std::vector<unsigned char> &rgba,const ImageSettings &settings)
{
    constexpr int W=864,H=480;
    if(rgba.size()!=W*H*4) return;
    /*
     * The resolution is a request to the server now, not a local
     * down-and-up resample: softening an already-decoded picture to
     * make it look like a smaller one cost two scaler passes and gave
     * nothing back. What is left here is the sharpening, which has no
     * server-side equivalent.
     */
    if(settings.sharpness) {
        auto original=rgba;
        for(int y=1;y<H-1;++y) for(int x=1;x<W-1;++x) for(int c=0;c<3;++c) {
            int p=(y*W+x)*4+c, center=original[p];
            int neighbours=original[p-4]+original[p+4]+original[p-W*4]+original[p+W*4];
            rgba[p]=std::clamp(center+(4*center-neighbours)*settings.sharpness/16,0,255);
        }
    }
}
