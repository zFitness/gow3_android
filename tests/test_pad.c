#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
static uint32_t published;
static int vibration_percent=100;
int gow3gpu_overlay_captures_input(void) { return capture; }
void gow3gpu_pad_publish(uint32_t buttons) { published=buttons; }
int gow3gpu_vibration_percent(void) { return vibration_percent; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    assert(scale_vibration(255, 100) == 65535);
    assert(scale_vibration(255, 50) == 32767);
    assert(scale_vibration(255, 0) == 0);
    assert(scale_vibration(255, 150) == 65535);

    char path[]="/tmp/gow3-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("SPARTAN_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    assert((data.buttons & BTN_TOUCHPAD));
    /* 裸 touchpad 仍默认左侧点击(x=480),与 touchpad_left 一致。 */
    inject(path,"touchpad");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert((data.buttons & BTN_TOUCHPAD));
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    /* The pad file carries the triggers as numbers: PadBridge omits the l2/r2 tokens whenever the
     * value is above 0, so a file with only l2=/r2= still has to move the analogue axes. */
    inject(path,"l2=128 r2=200");
    assert(pad_read_state(1,&data)==0 && data.l2==128 && data.r2==200);
    inject(path,"l2=999 r2=-5");
    assert(pad_read_state(1,&data)==0 && data.l2==255 && data.r2==0);
    /* 数值扳机越过 30 阈值时与物理手柄一致:模拟量与数字按键位同时有效。 */
    inject(path,"l2=255 r2=255");
    assert(pad_read_state(1,&data)==0 && data.l2==255 && data.r2==255);
    assert((data.buttons & (BTN_L2|BTN_R2))==(BTN_L2|BTN_R2));
    /* 阈值边界:30 不置位、31 置位,保护与 sample_host() 一致的 >30 判定。 */
    inject(path,"l2=30 r2=30");
    assert(pad_read_state(1,&data)==0 && data.l2==30 && data.r2==30);
    assert((data.buttons & (BTN_L2|BTN_R2))==0);
    inject(path,"l2=31 r2=31");
    assert(pad_read_state(1,&data)==0 && data.l2==31 && data.r2==31);
    assert((data.buttons & (BTN_L2|BTN_R2))==(BTN_L2|BTN_R2));
    /* 释放:注入源自身值归零(空文件或 l2=0),注入贡献的模拟量与按键位都被清除。 */
    inject(path,"l2=255");
    assert(pad_read_state(1,&data)==0 && data.l2==255 && (data.buttons & BTN_L2));
    inject(path,"l2=0");
    assert(pad_read_state(1,&data)==0 && data.l2==0 && !(data.buttons & BTN_L2));
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.l2==0 && data.r2==0);
    assert((data.buttons & (BTN_L2|BTN_R2))==0);
    /* 二进制 token 仍置位按键位并拉满模拟量(既有路径不回归)。 */
    inject(path,"cross l2 r2");
    assert(pad_read_state(1,&data)==0 && data.l2==255 && data.r2==255);
    assert((data.buttons & BTN_CROSS) && (data.buttons & (BTN_L2|BTN_R2))==(BTN_L2|BTN_R2));
    inject(path,"l2 r2");
    assert(pad_read_state(1,&data)==0 && data.l2==255 && data.r2==255);
    assert((data.buttons & (BTN_L2|BTN_R2))==(BTN_L2|BTN_R2));
    /* overlay 发布只基于 token 按键位:数值注入不进发布,二进制 token 进发布。 */
    inject(path,"l2=255");
    assert(pad_read_state(1,&data)==0 && (published & (BTN_L2|BTN_R2))==0);
    inject(path,"l2");
    assert(pad_read_state(1,&data)==0 && (published & BTN_L2));
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && published==0);

    /* While the overlay holds the input the game stays neutral, but the file is still read and
     * published: on Android that file is the only thing the save list can hear. */
    capture=1;
    inject(path,"cross circle");
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==0 && data.left_x==128 && data.left_y==128);
    assert(published==(BTN_CROSS|BTN_CIRCLE));
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && published==0);

    /* Cross accepted the dialog and is still held: the game must not see it until it is released. */
    inject(path,"cross");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    capture=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    inject(path,"cross");
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS));

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="gow3 test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, "
         "pad-file triggers, publish while the overlay holds input, hold-after-capture release");
}
