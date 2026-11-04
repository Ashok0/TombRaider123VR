// Shared production-hook scenarios for TR1-5; Render returns world joint frames.
template<class Render,class MoveController,class MoveAnimation>
void ExerciseSidearmAnimation(Render render,MoveController moveController,MoveAnimation moveAnimation) {
    using namespace tr;using namespace tr::motiongun;
    auto sameArms=[](const auto& a,const auto& b,const char* reason) {
        for(int j=8;j<14;++j) {
            Check(firstperson::Length(Sub(a[j].origin,b[j].origin))<.12f,reason);
            for(int r=0;r<3;++r) for(int c=0;c<3;++c)
                Check(std::fabs(a[j].basis.r[r][c]-b[j].basis.r[r][c])<.002f,reason);
        }
    };
    auto onControllers=[](const auto& pose) {
        for(int hand=0;hand<2;++hand) {
            Frame wrist{};Basis controller{};BuildControllerWrist(hand,wrist,controller);
            Check(firstperson::Length(Sub(pose[hand?10:13].origin,wrist.origin))<.08f,
                  "completed equip/unequip blend restores exact controller wrists");
        }
    };
    g_pistolAnimation.Reset();
    const auto standing=render(0,false);onControllers(standing);
    const auto drawing=render(2,false);
    sameArms(standing,drawing,"drawing starts at the last displayed pose without an arm flash");
    Check(g_pistolAnimation.blending && g_pistolAnimation.mode==2,"button draw starts the smooth native-animation handoff");
    g_pistolAnimation.started-=.20;
    const auto native=render(2,false);
    Check(!g_pistolAnimation.blending,"draw handoff completes");
    moveController();
    sameArms(native,render(2,false),"moving controllers cannot pull animated hands away from the holsters");
    moveAnimation();
    const auto advanced=render(2,false);
    Check(firstperson::Length(Sub(advanced[10].origin,native[10].origin))>5,
          "native draw animation visibly moves the sidearm hand");
    sameArms(advanced,render(2,true),"stereo and attachment redraws cannot advance the animation blend");
    const auto ready=render(4,false);
    sameArms(advanced,ready,"ready state starts from the last animated arm pose");
    g_pistolAnimation.started-=.20;onControllers(render(4,false));
    const auto tracked=render(4,false),holstering=render(3,false);
    sameArms(tracked,holstering,"holstering starts at the current controller pose without snapping");
    g_pistolAnimation.started-=.20;
    const auto nativeHolster=render(3,false);
    moveController();
    sameArms(nativeHolster,render(3,false),"native holster motion retains ownership until unequip completes");
    moveAnimation();const auto endHolster=render(3,false);
    Check(firstperson::Length(Sub(endHolster[10].origin,nativeHolster[10].origin))>5,
          "native unequip animation visibly moves the sidearm hand");
    sameArms(endHolster,render(0,false),"empty hands return smoothly from the last holster pose");
    g_pistolAnimation.started-=.20;onControllers(render(0,false));
    render(2,false);g_pistolAnimation.started-=.08;
    const auto mid=render(2,false),reversed=render(3,false);
    sameArms(mid,reversed,"reversing an equip transition preserves the visible pose");
}
