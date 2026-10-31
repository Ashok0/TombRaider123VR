namespace {
void TestWorldInventory() {
    using namespace tr;
    using inventory::Input;
    using inventory::Session;
    for(int which:{0,1,2}) for(bool moving:{false,true}) {
        Session s;
        bool owned=false;
        const Input opening{XB_BACK,moving ? .7f : 0,0};
        auto in=opening;
        Check(s.Update(in,true,false,which,100,.5f,owned) && !owned && in.buttons==XB_BACK && in.x==opening.x,
              "X/BACK passes unchanged to native menu handling while anchor is prepared");
        for(uint64_t now:{110,150,250,300}) {
            in=opening;
            Check(!s.Update(in,true,false,which,now,.5f,owned) && !owned &&
                  in.buttons==XB_BACK && in.x==opening.x,
                  "held native X keeps its original timing without synthetic menu pulses");
        }
        in=opening;s.Update(in,true,true,which,350,.5f,owned);
        Check(s.open && !s.pending && !in.buttons && !in.x && !in.y,
              "opening X press does not immediately close native inventory");
        in={};s.Update(in,true,true,which,360,.5f,owned);
        for(auto direction:{Input{0,1,.2f},Input{0,-1,.2f},Input{0,.2f,1},Input{0,.2f,-1}}) {
            in=direction;s.Update(in,true,true,which,370,.5f,owned);
            const uint16_t expected=direction.x==1 ? XB_DPAD_RIGHT : direction.x==-1 ? XB_DPAD_LEFT :
                direction.y==1 ? XB_DPAD_UP : XB_DPAD_DOWN;
            Check(in.buttons==expected && !in.x && !in.y && owned,
                  "unshifted LS navigates exactly one native inventory direction");
        }
        for(uint16_t button:{uint16_t(XB_A),uint16_t(XB_B),uint16_t(XB_BACK)}) {
            in={button};s.Update(in,true,true,which,380,.5f,owned);
            Check(in.buttons==button && owned,"A selects, B backs out and X/BACK retains native close");
        }
        in={XB_Y,0,0,255,255};s.Update(in,true,true,which,390,.5f,owned);
        Check(!in.lt && !in.rt && owned,"menu input bypasses gun, graphics and view chords");
        in={XB_START|XB_RIGHT_THUMB};s.Update(in,true,true,which,395,.5f,owned);
        Check(!in.buttons,"menu chord and zoom cannot leak into a spatial inventory session");
        in={XB_A|XB_B,1,0,255,255};s.Update(in,true,false,which,400,.5f,owned);
        Check(!s.open && owned && !in.buttons && !in.x && !in.lt && !in.rt,
              "held select/back/trigger/stick cannot jump roll shoot or move on close");
        in={};s.Update(in,true,false,which,410,.5f,owned);
        in={XB_A,1,0,0,255};s.Update(in,true,false,which,420,.5f,owned);
        Check(!owned && in.buttons==XB_A && in.x==1 && in.rt==255,
              "fresh gameplay input works after release drain");
        in=opening;Check(s.Update(in,true,false,which,430,.5f,owned),"fresh X can prepare another session");
        in=opening;s.Update(in,true,false,which,1500,.5f,owned);
        Check(!s.open && !s.pending && !owned && in.buttons==XB_BACK && in.x==opening.x,
              "rejected menu press times out without consuming input or starting an exit drain");
        in={};s.Update(in,true,false,which,1510,.5f,owned);
        in={XB_B};s.Update(in,true,false,which,1520,.5f,owned);
        Check(!owned && in.buttons==XB_B,"rejected open releases gameplay controls");
    }
    for(int guard=0;guard<7;++guard) {
        Session s;bool owned=false;
        Input in{XB_BACK};
        if(guard==2) in.buttons|=XB_LEFT_THUMB|XB_RIGHT_THUMB;
        if(guard==3) in={XB_DPAD_DOWN};
        if(guard==4) in={XB_RIGHT_THUMB,0,-1};
        if(guard==5) in={XB_DPAD_DOWN,0,0,0,0,true};
        if(guard==6) in={XB_X}; // XInput X is Walk, not the Touch X menu button.
        Check(!s.Update(in,guard!=0,guard==1,0,100,.5f,owned) && !owned,
              "disabled, existing menus, Photo chord, old R3 shortcuts and Walk do not prepare inventory");
    }
    for(int which:{0,1,2}) {
        Session s;bool owned=false;
        // Native game keeps inventory closed when X switches ammo. Check
        // repeated presses, held fire, movement and the pending timeout.
        for(uint64_t now:{100,110,120,140,150,160,1200,1300}) {
            Input in{uint16_t(now==120 || now==150 ? 0 : XB_BACK),.7f,-.3f,255,255};
            const auto before=in;
            s.Update(in,true,false,which,now,.5f,owned);
            Check(!owned && !s.open && !s.release && in.buttons==before.buttons &&
                  in.x==before.x && in.y==before.y && in.lt==255 && in.rt==255,
                  "rejected menu request never opens spatial inventory or interrupts triggers and movement");
        }
        s.Sync(true,true,which,2400);
        Check(!s.open,"expired ammo press cannot claim an unrelated later menu");
    }

}
}
