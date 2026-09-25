#include "pad_menu.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <thread>
#include <unistd.h>

static drc::InputData Sample(float x=0,float y=0)
{ drc::InputData in; in.valid=true; in.right_stick_x=x; in.right_stick_y=y; return in; }
static drc::InputData Both(float x,float y)
{ auto in=Sample(x,y); in.left_stick_x=x; in.left_stick_y=y; return in; }
static drc::InputData Tap(PadMenu &menu,float x,float y)
{
    auto in=Sample(); menu.Filter(in);
    in=Sample(); in.ts_pressed=true; in.ts_x=x; in.ts_y=y; menu.Filter(in); return in;
}
int main()
{
    float x=.05,y=-.06; PadMenu::Correct(x,y,0,0,.12); assert(x==0 && y==0);
    x=.12001; y=0; PadMenu::Correct(x,y,0,0,.12); assert(x>0 && x<.001);
    x=1; y=0; PadMenu::Correct(x,y,.1,-.05,.12); assert(x>.99);
    x=-1; y=0; PadMenu::Correct(x,y,.1,0,.12); assert(x==-1);
    x=.1; y=-.05; PadMenu::Correct(x,y,.1,-.05,.12); assert(x==0 && y==0);
    std::string path="/tmp/bs-pad-menu-test-"+std::to_string(getpid())+"/settings";
    PadMenu menu(path);
    auto in=Tap(menu,1,0); assert(!in.ts_pressed); // opening gesture consumed
    in=Sample(.5,.5); in.ts_pressed=true; in.ts_x=.5; in.ts_y=.5;
    menu.Filter(in); assert(!in.ts_pressed && in.right_stick_x==0);
    // A calibration learns a stable off-centre rest position.
    Tap(menu,150.f/864,150.f/480);
    for(int i=0;i<320;i++) {
        in=Sample(.15,-.08); menu.Filter(in);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Move the trigger to bottom-left and persist it.
    Tap(menu,500.f/864,437.f/480);
    std::vector<unsigned char> rgba;
    menu.Draw(rgba);
    std::ofstream preview("/tmp/pad-menu-preview.ppm",std::ios::binary);
    preview << "P6\n864 480\n255\n";
    for(size_t i=0;i<rgba.size();i+=4) preview.write((char*)&rgba[i],3);
    preview.close();
    in=Tap(menu,760.f/864,45.f/480); assert(!in.ts_pressed); // closing tap consumed
    // Held touch cannot leak into the game after closing.
    in=Sample(); in.ts_pressed=true; in.ts_x=.5; in.ts_y=.5; menu.Filter(in);
    assert(!in.ts_pressed);
    in=Sample(.15,-.08); menu.Filter(in); assert(in.right_stick_x==0 && in.right_stick_y==0);
    PadMenu loaded(path);
    in=Tap(loaded,1,0); assert(in.ts_pressed); // previous corner now belongs to the game
    in=Tap(loaded,0,1); assert(!in.ts_pressed); // new corner persists
    in=Tap(loaded,760.f/864,45.f/480); assert(!in.ts_pressed);
    in=Sample(.15,-.08); loaded.Filter(in); assert(in.right_stick_x==0 && in.right_stick_y==0);
    // Dragging into the corner does not unexpectedly open the menu.
    in=Tap(loaded,.5,.5); assert(in.ts_pressed);
    in=Sample(); in.ts_pressed=true; in.ts_x=0; in.ts_y=1; loaded.Filter(in); assert(in.ts_pressed);
    // Opening the menu suppresses held buttons through closing until release.
    Tap(loaded,0,1);
    in=Sample(); in.buttons=drc::InputData::kBtnA; loaded.Filter(in); assert(!in.buttons);
    in=Sample(); in.buttons=drc::InputData::kBtnA; in.ts_pressed=true;
    in.ts_x=760.f/864; in.ts_y=45.f/480; loaded.Filter(in); assert(!in.buttons);
    in=Sample(); in.buttons=drc::InputData::kBtnA; loaded.Filter(in); assert(!in.buttons);
    in=Sample(); loaded.Filter(in);
    in.buttons=drc::InputData::kBtnA; loaded.Filter(in); assert(in.buttons==drc::InputData::kBtnA);
    // The left stick has its own deadzone, and starts without one.
    //
    // The right stick's 12% is there because this pad's right stick
    // drifts; applying that to the left as well would have been a
    // silent change to a stick nobody complained about. So the default
    // is nothing, and the two are checked against each other -- one
    // deflection, small enough for the right stick to swallow, must
    // still reach the game through the left.
    in=Both(.05,0); loaded.Filter(in);
    assert(in.right_stick_x==0 && in.left_stick_x>.04);

    Tap(loaded,0,1);                        // open
    Tap(loaded,100.f/864,100.f/480);        // select the left stick
    for(int i=0;i<20;i++) Tap(loaded,332.f/864,278.f/480);  // 20% deadzone
    in=Tap(loaded,760.f/864,45.f/480);      // close
    in=Sample(); loaded.Filter(in);         // release the held touch
    in=Both(.15,0); loaded.Filter(in); assert(in.left_stick_x==0);
    in=Both(.5,0); loaded.Filter(in);
    assert(in.left_stick_x>.3 && in.left_stick_x<.5);  // rescaled, not clipped
    // And the right stick still has its own centre and its own 12%,
    // which is the whole point of the two being separate.
    assert(in.right_stick_x>.2);
    in=Both(.05,0); loaded.Filter(in); assert(in.right_stick_x==0);

    { PadMenu again(path); auto probe=Both(.15,0); again.Filter(probe);
      assert(probe.left_stick_x==0); }      // 20% survived the restart

    // The screen button reports its change once, and only once: the
    // loop that owns the connection acts on it, and acting on it twice
    // is two round trips and a rebuilt encoder for nothing.
    int want = -1;
    assert(!loaded.TakeScreenChange(&want));
    Tap(loaded,0,1);                        // open
    Tap(loaded,626.f/864,45.f/480);         // top screen
    assert(loaded.TakeScreenChange(&want) && want==1);
    assert(!loaded.TakeScreenChange(&want));
    Tap(loaded,626.f/864,45.f/480);         // and back
    assert(loaded.TakeScreenChange(&want) && want==0);
    // A screen set by the caller is not reported back to it.
    loaded.SetScreen(1);
    assert(!loaded.TakeScreenChange(&want));
    in=Tap(loaded,760.f/864,45.f/480);      // close

    unlink(path.c_str()); rmdir(path.substr(0,path.rfind('/')).c_str());
    puts("PASS: both sticks' deadzones, full range, calibration, persistent corner, touch/button isolation");
}
