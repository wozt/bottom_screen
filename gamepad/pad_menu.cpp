#include "pad_menu.h"
#include <cairo/cairo.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {
constexpr int W = 864, H = 480;
bool Hit(float x, float y, int bx, int by, int bw, int bh)
{ return x >= bx && x < bx+bw && y >= by && y < by+bh; }
float Safe(float v) { return std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0; }
}

std::string PadMenu::DefaultPath()
{
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    return std::string(config && *config ? config : home ? std::string(home)+"/.config" : ".")
        + "/bottom-screen/gamepad.conf";
}
PadMenu::PadMenu(std::string path) : path_(std::move(path))
{
    std::ifstream file(path_);
    std::string key; float value;
    while (file >> key >> value) {
        if (!std::isfinite(value)) continue;
        if (key == "bitrate" && value >= 0 && value <= 7) image_.bitrate = int(value);
        /*
         * A new key, deliberately. "detail" used to number a local blur
         * -- 0 meaning none -- and now numbers a resolution, where 0 is
         * half the panel. Reading the old key would have quietly halved
         * the picture of everybody who had ever opened this menu.
         */
        if (key == "resolution" && value >= 0 && value < ImageSettings::kDetailCount)
            detail_[0] = int(value);
        if (key == "top_resolution" && value >= 0 && value < ImageSettings::kDetailCount)
            detail_[1] = int(value);
        if (key == "filter" && value >= 0 && value <= 3) image_.filter = int(value);
        if (key == "sharpness" && value >= 0 && value <= 4) image_.sharpness = int(value);
        if (key == "delay" && value >= 0 && value <= 5) image_.delay = int(value);
        if (key == "corner" && value >= 0 && value <= 3) corner_ = int(value);
        // The right stick's three keys keep the names they were written
        // under, so a settings file from before the left one existed
        // still loads and still means the same thing.
        if (key == "center_x" && fabs(value) <= .4) cx_[kRight] = value;
        if (key == "center_y" && fabs(value) <= .4) cy_[kRight] = value;
        if (key == "deadzone" && value >= 0 && value <= .4) deadzone_[kRight] = value;
        if (key == "left_center_x" && fabs(value) <= .4) cx_[kLeft] = value;
        if (key == "left_center_y" && fabs(value) <= .4) cy_[kLeft] = value;
        if (key == "left_deadzone" && value >= 0 && value <= .4) deadzone_[kLeft] = value;
    }
}
void PadMenu::Save()
{
    std::error_code error;
    auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    std::string tmp = path_ + ".tmp." + std::to_string(getpid());
    std::ofstream file(tmp);
    file << "corner " << corner_
         << "\ncenter_x " << cx_[kRight] << "\ncenter_y " << cy_[kRight]
         << "\ndeadzone " << deadzone_[kRight]
         << "\nleft_center_x " << cx_[kLeft] << "\nleft_center_y " << cy_[kLeft]
         << "\nleft_deadzone " << deadzone_[kLeft] << '\n';
    file << "bitrate " << image_.bitrate
         << "\nresolution " << detail_[0] << "\ntop_resolution " << detail_[1]
         << "\nfilter " << image_.filter << "\nsharpness " << image_.sharpness
         << "\ndelay " << image_.delay << '\n';
    file.close();
    if (!error && file) std::filesystem::rename(tmp, path_, error);
    if (error || !file) {
        message_ = "Échec de sauvegarde : vérifier le dossier de configuration.";
        std::filesystem::remove(tmp, error);
    }
}
ImageSettings PadMenu::Image()
{
    std::lock_guard<std::mutex> guard(mutex_);
    ImageSettings out = image_;
    out.detail = detail_[screen_];
    return out;
}
void PadMenu::SetFit(int w, int h)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (w > 0 && h > 0) { fit_w_ = w; fit_h_ = h; }
}
void PadMenu::Override(int bottom_res, int top_res, int filter, int sharpness)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto set = [](int &target, int value, int limit) {
        if (value >= 0 && value <= limit) target = value;
    };
    set(detail_[0], bottom_res, ImageSettings::kDetailCount-1);
    set(detail_[1], top_res, ImageSettings::kDetailCount-1);
    set(image_.filter, filter, 3);
    set(image_.sharpness, sharpness, 4);
}
void PadMenu::SetScreen(int screen)
{
    std::lock_guard<std::mutex> guard(mutex_);
    screen_ = screen ? 1 : 0;
    screen_changed_ = false;    // the caller already knows: it set it
}
bool PadMenu::TakeScreenChange(int *screen)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!screen_changed_) return false;
    screen_changed_ = false;
    *screen = screen_;
    return true;
}
void PadMenu::Correct(float &x, float &y, float cx, float cy, float deadzone)
{
    x = Safe(x); y = Safe(y);
    x = (x-cx) / (x >= cx ? 1-cx : 1+cx);
    y = (y-cy) / (y >= cy ? 1-cy : 1+cy);
    float radius = std::hypot(x, y);
    if (radius <= deadzone || radius == 0) { x = y = 0; return; }
    float scale = (std::min(radius, 1.f)-deadzone) / ((1-deadzone)*radius);
    x *= scale; y *= scale;
}
void PadMenu::Tap(float x, float y)
{
    if (Hit(x,y,704,22,136,46)) { open_ = false; calibrating_ = false; return; }
    if (Hit(x,y,556,22,140,46)) {
        screen_ = screen_ ? 0 : 1;
        screen_changed_ = true;
        message_ = screen_ ? "Écran du haut demandé." : "Écran du bas demandé.";
        return;
    }
    if (Hit(x,y,300,22,120,46)) { image_page_ = false; calibrating_ = false; return; }
    if (Hit(x,y,428,22,120,46)) { image_page_ = true;  calibrating_ = false; return; }
    if (image_page_) {
        int *values[]={&image_.bitrate,&detail_[screen_],&image_.filter,
                       &image_.sharpness,&image_.delay};
        const int limits[]={7,ImageSettings::kDetailCount-1,3,4,5};
        for(int row=0;row<5;++row) {
            int direction=Hit(x,y,224,96+row*58,48,42)?-1:Hit(x,y,400,96+row*58,48,42)?1:0;
            if(direction) { *values[row]=std::clamp(*values[row]+direction,0,limits[row]); Save(); }
        }
        auto preset=[&](int rate,int res,int filter,int sharp){
            image_.bitrate=rate; image_.filter=filter; image_.sharpness=sharp;
            image_.delay=0; detail_[screen_]=res; Save();
        };
        if(Hit(x,y,34,411,250,44)) preset(2,1,0,0);
        if(Hit(x,y,304,411,250,44)) preset(4,2,2,0);
        if(Hit(x,y,574,411,250,44)) preset(6,3,2,1);
        return;
    }
    for (int i = 0; i < 2; ++i) if (Hit(x,y,34+i*170,84,160,38)) {
        if (stick_ != i) { stick_ = i; calibrating_ = false; }
        return;
    }
    if (Hit(x,y,34,130,330,54)) {
        calibration_start_ = std::chrono::steady_clock::now();
        calibrating_ = true; samples_ = 0; sum_x_ = sum_y_ = 0;
        min_x_ = min_y_ = 1; max_x_ = max_y_ = -1;
        message_ = "Ne touchez pas au stick pendant 1,5 seconde…";
    } else if (!calibrating_ && Hit(x,y,34,254,64,48)) {
        deadzone_[stick_] = std::max(0.f, deadzone_[stick_]-.01f); Save();
    } else if (!calibrating_ && Hit(x,y,300,254,64,48)) {
        deadzone_[stick_] = std::min(.4f, deadzone_[stick_]+.01f); Save();
    } else if (!calibrating_ && Hit(x,y,34,319,330,44)) {
        cx_[stick_] = cy_[stick_] = 0;
        deadzone_[stick_] = stick_ == kRight ? .12f : 0.f;
        char line[96];
        snprintf(line,sizeof(line),"Correction remise à zéro (zone morte : %.0f %%).",
                 deadzone_[stick_]*100);
        message_ = line; Save();
    }
    for (int i = 0; i < 4; ++i) if (Hit(x,y,34+i*202,417,190,42)) {
        corner_ = i; Save();
    }
}
void PadMenu::Filter(drc::InputData &in)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!in.valid) {
        touched_ = false; consumed_ = false;
        if (calibrating_) { calibrating_ = false; message_ = "Calibration annulée : GamePad déconnecté."; }
        return;
    }
    raw_x_[kLeft] = Safe(in.left_stick_x);   raw_y_[kLeft] = Safe(in.left_stick_y);
    raw_x_[kRight] = Safe(in.right_stick_x); raw_y_[kRight] = Safe(in.right_stick_y);
    bool down = in.ts_pressed && !touched_;
    if (!in.ts_pressed) consumed_ = false;
    if (down) {
        if (open_) { consumed_ = true; Tap(in.ts_x*W, in.ts_y*H); }
        else {
            bool right = corner_ & 1, bottom = corner_ & 2;
            bool hit_x = right ? in.ts_x >= 1-8.f/854 : in.ts_x < 8.f/854;
            bool hit_y = bottom ? in.ts_y >= 1-8.f/480 : in.ts_y < 8.f/480;
            if (hit_x && hit_y) { open_ = true; consumed_ = true; }
        }
    }
    touched_ = in.ts_pressed;
    if (calibrating_) {
        auto elapsed = std::chrono::steady_clock::now()-calibration_start_;
        if (elapsed >= std::chrono::milliseconds(300)) {
            sum_x_ += raw_x_[stick_]; sum_y_ += raw_y_[stick_]; samples_++;
            min_x_ = std::min(min_x_,raw_x_[stick_]); max_x_ = std::max(max_x_,raw_x_[stick_]);
            min_y_ = std::min(min_y_,raw_y_[stick_]); max_y_ = std::max(max_y_,raw_y_[stick_]);
        }
        if (elapsed >= std::chrono::milliseconds(1500)) {
            calibrating_ = false;
            if (samples_ >= 60 && max_x_-min_x_ < .08 && max_y_-min_y_ < .08 &&
                fabs(sum_x_/samples_) <= .4 && fabs(sum_y_/samples_) <= .4) {
                cx_[stick_] = sum_x_/samples_; cy_[stick_] = sum_y_/samples_;
                message_ = "Centre calibré et sauvegardé."; Save();
            } else message_ = "Stick instable ou trop incliné : relâchez-le et réessayez.";
        }
    }
    for (int i = 0; i < 2; ++i) {
        out_x_[i] = raw_x_[i]; out_y_[i] = raw_y_[i];
        Correct(out_x_[i],out_y_[i],cx_[i],cy_[i],deadzone_[i]);
    }
    unsigned buttons = in.buttons;
    suppressed_ &= buttons;
    if (open_ || consumed_) suppressed_ |= buttons;
    in.buttons = static_cast<drc::InputData::ButtonMask>(buttons & ~suppressed_);
    in.left_stick_x = out_x_[kLeft];   in.left_stick_y = out_y_[kLeft];
    in.right_stick_x = out_x_[kRight]; in.right_stick_y = out_y_[kRight];
    if (open_ || consumed_) {
        in.left_stick_x = in.left_stick_y = in.right_stick_x = in.right_stick_y = 0;
        in.ts_pressed = false;
    }
}

void PadMenu::Draw(std::vector<unsigned char> &rgba)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (rgba.size() != W*H*4) rgba.assign(W*H*4,0);
    if (!open_) {
        int bx = corner_&1 ? W-8 : 0, by = corner_&2 ? H-8 : 0;
        for (int y=by; y<by+8; ++y) for (int x=bx; x<bx+8; ++x) {
            auto p = &rgba[(y*W+x)*4]; p[0]=100; p[1]=205; p[2]=220; p[3]=255;
        }
        return;
    }
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,W,H);
    auto cr = cairo_create(surface);
    auto rect = [&](int x,int y,int w,int h,double r,double g,double b) {
        cairo_set_source_rgb(cr,r,g,b); cairo_rectangle(cr,x,y,w,h); cairo_fill(cr);
    };
    auto text = [&](double x,double y,const char *label,int size=18) {
        cairo_set_source_rgb(cr,.92,.95,1); cairo_set_font_size(cr,size);
        cairo_move_to(cr,x,y); cairo_show_text(cr,label);
    };
    auto button = [&](int x,int y,int w,int h,const char *label,bool selected=false) {
        rect(x,y,w,h,selected?.08:.15,selected?.45:.23,selected?.5:.31);
        cairo_text_extents_t ext; cairo_set_font_size(cr,18); cairo_text_extents(cr,label,&ext);
        text(x+(w-ext.width)/2-ext.x_bearing,y+(h-ext.height)/2-ext.y_bearing,label);
    };
    rect(0,0,W,H,.045,.07,.11);
    cairo_select_font_face(cr,"sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    /*
     * A title that says where you are, and a line under it that says
     * what you are looking at -- which screen, and at what size. Two
     * named pages rather than one button labelled with the page you are
     * not on: that reads as a label until you press it.
     */
    text(34,42,"Réglages GamePad",22);
    {
        ImageSettings shown = image_; shown.detail = detail_[screen_];
        char where[96];
        snprintf(where,sizeof(where),"Écran %s · %d × %d",
                 screen_?"du haut":"du bas",
                 fit_w_*shown.Num()/shown.Den(), fit_h_*shown.Num()/shown.Den());
        cairo_set_source_rgb(cr,.62,.70,.78); cairo_set_font_size(cr,15);
        cairo_move_to(cr,34,66); cairo_show_text(cr,where);
    }
    button(300,22,120,46,"Manette",!image_page_);
    button(428,22,120,46,"Image",image_page_);
    button(556,22,140,46,screen_?"Voir le bas":"Voir le haut");
    button(704,22,136,46,"Fermer");
    if (image_page_) {
        char res_label[64];
        /* The label column is 34..218: about twenty characters at this
         * size, and anything longer runs under the minus button. The
         * subtitle already says which screen, so this one need not. */
        snprintf(res_label,sizeof(res_label),"Résolution %s",
                 screen_?"(haut)":"(bas)");
        const char *labels[]={"Débit demandé",res_label,
                              "Filtre d’image","Netteté","Délai d’image"};
        const char *rates[]={"Auto","2 Mb/s","4 Mb/s","6 Mb/s","8 Mb/s","12 Mb/s","16 Mb/s","24 Mb/s"};
        const char *filters[]={"Bilinéaire","Bicubique","Lanczos","Pixels"};
        char detail[48],sharp[32],delay[32];
        {
            ImageSettings shown=image_; shown.detail=detail_[screen_];
            const int dw=fit_w_*shown.Num()/shown.Den();
            const int dh=fit_h_*shown.Num()/shown.Den();
            if(shown.Num()==shown.Den())
                snprintf(detail,sizeof(detail),"%d × %d",dw,dh);
            else
                snprintf(detail,sizeof(detail),"%d × %d  (×%.2g)",dw,dh,
                         double(shown.Num())/shown.Den());
        }
        snprintf(sharp,sizeof(sharp),"%d %%",image_.sharpness*25);
        snprintf(delay,sizeof(delay),"%d ms",image_.delay*20);
        const char *values[]={rates[image_.bitrate],detail,filters[image_.filter],sharp,delay};
        for(int row=0;row<5;++row) {
            int y=96+row*58;
            text(34,y+27,labels[row],16);
            button(224,y,48,42,"-"); button(400,y,48,42,"+");
            text(282,y+27,values[row],16);
        }
        text(480,108,"Aperçu en direct",20);
        // Preview uses the processed game frame, never the menu pixels.
        std::vector<unsigned char> preview(W*H*4);
        for(size_t p=0;p<preview.size();p+=4) {
            preview[p]=rgba[p+2]; preview[p+1]=rgba[p+1]; preview[p+2]=rgba[p]; preview[p+3]=255;
        }
        auto view=cairo_image_surface_create_for_data(preview.data(),CAIRO_FORMAT_ARGB32,W,H,W*4);
        cairo_save(cr); cairo_translate(cr,480,125); cairo_scale(cr,350.0/W,194.0/H);
        cairo_set_source_surface(cr,view,0,0); cairo_paint(cr); cairo_restore(cr);
        cairo_surface_destroy(view);
        text(480,343,"×1 = la taille de la dalle. Au-dessus : plus net, mais",14);
        text(480,363,"seulement si l’émulateur rend plus grand que ça.",14);
        text(480,391,"Chaque écran garde sa propre résolution.",14);

        text(34,397,"Le délai retarde l’image, pas le son. 0 ms = au plus vite.",15);
        button(34,411,250,44,"Réactif"); button(304,411,250,44,"Équilibré"); button(574,411,250,44,"Détaillé");
        text(34,474,"Le débit est partagé avec les autres clients du même écran.",13);
    } else {
    // One panel, two sticks: the selector says which one every control
    // below it acts on, so there is no second copy of the same six
    // widgets to keep in step.
    text(34,100,"Régler le stick :",16);
    button(174,84,92,38,"gauche",stick_==kLeft);
    button(272,84,92,38,"droit",stick_==kRight);
    button(34,130,330,54,calibrating_?"Calibration en cours…":"Calibrer le centre");
    char line[160];
    snprintf(line,sizeof(line),"Centre : X %+.3f   Y %+.3f",cx_[stick_],cy_[stick_]);
    text(34,212,line);
    text(34,242,"Zone morte (ignorée autour du centre)",15);
    button(34,254,64,48,"-"); button(300,254,64,48,"+");
    snprintf(line,sizeof(line),"%.0f %%",deadzone_[stick_]*100); text(170,286,line,24);
    button(34,319,330,44,"Remettre ce stick à zéro");
    // Raw position (amber) and corrected position (cyan), same stick space.
    rect(452,103,190,190,.11,.16,.21);
    cairo_set_line_width(cr,1); cairo_set_source_rgb(cr,.35,.42,.48);
    cairo_move_to(cr,547,103); cairo_line_to(cr,547,293);
    cairo_move_to(cr,452,198); cairo_line_to(cr,642,198); cairo_stroke(cr);
    cairo_arc(cr,547+cx_[stick_]*90,198-cy_[stick_]*90,deadzone_[stick_]*90,0,6.283185);
    cairo_stroke(cr);
    auto dot = [&](float x,float y,double r,double g,double b) {
        cairo_set_source_rgb(cr,r,g,b); cairo_arc(cr,547+x*90,198-y*90,5,0,6.283185); cairo_fill(cr);
    };
    dot(raw_x_[stick_],raw_y_[stick_],1,.65,.25);
    dot(out_x_[stick_],out_y_[stick_],.25,.9,1);
    char legend[64];
    snprintf(legend,sizeof(legend),"Stick %s",stick_==kLeft?"gauche":"droit");
    text(666,119,legend,17);
    text(666,151,"Brut : orange",16); text(666,183,"Envoyé : cyan",16);
    snprintf(line,sizeof(line),"Brut     X %+.3f   Y %+.3f",
             raw_x_[stick_],raw_y_[stick_]); text(440,323,line,17);
    snprintf(line,sizeof(line),"Envoyé  X %+.3f   Y %+.3f",
             out_x_[stick_],out_y_[stick_]); text(440,350,line,17);
    text(34,381,message_.c_str(),16);
    text(34,406,"Coin à toucher pour ouvrir ce menu (carré de 8 pixels)",14);
    const char *corners[] = {"Haut gauche","Haut droite","Bas gauche","Bas droite"};
    for (int i=0;i<4;++i) button(34+i*202,417,190,42,corners[i],corner_==i);
    }
    cairo_destroy(cr); cairo_surface_flush(surface);
    auto pixels = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
        auto src = pixels+y*stride+x*4, dst = rgba.data()+(y*W+x)*4;
        dst[0]=src[2]; dst[1]=src[1]; dst[2]=src[0]; dst[3]=255;
    }
    cairo_surface_destroy(surface);
}
