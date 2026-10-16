#include "unity.h"
#include "kvm_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 88172645463325252ULL;
static uint32_t rnd(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return (uint32_t)(rs>>11); }

static int viol[16]; static char first[16][256];
#define VIOL(id, ...) do { viol[id]++; if(!first[id][0]) snprintf(first[id],256,__VA_ARGS__); } while(0)

void test_fuzz_invariants(void) {
    int seeds = 200;
    long steps = 20000;
    for(int sd=1; sd<=seeds; sd++){
        rs = 88172645463325252ULL ^ ((uint64_t)sd*0x9E3779B97F4A7C15ULL);
        kvm_logic_config_t c;
        kvm_logic_config_default(&c);
        c.led_stable_time_ms=50; c.switch_kvm_pulse_time_ms=150; c.switch_led_wait_timeout_ms=2000;
        c.switch_video_sync_timeout_ms=3000; c.setup_time_ms=10; c.max_power_pulse_ms=500;
        c.max_reset_pulse_ms=200; c.max_force_power_pulse_ms=5000; c.no_signal_settle_ms=500;

        kvm_logic_t L;
        TEST_ASSERT_EQUAL(KVM_RESULT_OK, kvm_logic_init(&L, &c));
        kvm_logic_inputs_t in = { .led_raw_state=1, .video_sync=KVM_VIDEO_SYNC_LOCKED };
        kvm_logic_outputs_t prev, cur; memset(&prev,0,sizeof prev); prev.a0a1_channel=1; prev.atx_locked=1;
        uint64_t now = 1000, a0a1_changed_at = 1000, p_start=0, r_start=0, k_start=0;
        int p_on=0,r_on=0,k_on=0; uint32_t last_dt=1;

        for(long i=0;i<steps;i++){
            uint32_t dt = 1 + rnd()%30; last_dt=dt; now += dt;
            uint32_t x = rnd()%100;
            if(x<6) in.led_raw_state = (uint8_t)(rnd()%7);
            else if(x<8) in.led_raw_state = (uint8_t)(1+rnd()%4);
            if(rnd()%50==0) in.video_sync = (kvm_video_sync_state_t)(rnd()%3);
            x = rnd()%100;
            if(x<4) kvm_logic_request_switch(&L,(kvm_channel_t)(rnd()%6));
            else if(x<10){
                kvm_channel_t ch = (rnd()%4)? cur.active_channel : (kvm_channel_t)(rnd()%6);
                kvm_atx_target_t target = KVM_ATX_TARGET_POWER;
                int tgt_rnd = rnd() % 3;
                if (tgt_rnd == 1) target = KVM_ATX_TARGET_RESET;
                if (tgt_rnd == 2) target = KVM_ATX_TARGET_POWER_FORCE;

                kvm_logic_request_atx(&L,ch,target, rnd()%7000);
            }
            kvm_logic_step(&L,&in,now,&cur);
            int anyE = cur.e_power_low||cur.e_reset_low||cur.e_kvm_low, pE=prev.e_power_low||prev.e_reset_low||prev.e_kvm_low;

            /* C1 ATX low while locked */
            if((cur.e_power_low||cur.e_reset_low) && cur.atx_locked) VIOL(1,"seed%d step%ld: ATX E low while atx_locked (state=%d)",sd,i,L.state);
            /* C2 two enables low simultaneously */
            if(cur.e_kvm_low && (cur.e_power_low||cur.e_reset_low)) VIOL(2,"seed%d step%ld: KVM and ATX enables low together",sd,i);
            /* C3 A0/A1 change while any E low (prev or cur) */
            if(cur.a0a1_channel!=prev.a0a1_channel){ a0a1_changed_at=now;
                if(pE||anyE) VIOL(3,"seed%d step%ld: A0/A1 %d->%d while E low (prev p/r/k=%d%d%d cur=%d%d%d state=%d)",sd,i,prev.a0a1_channel,cur.a0a1_channel,prev.e_power_low,prev.e_reset_low,prev.e_kvm_low,cur.e_power_low,cur.e_reset_low,cur.e_kvm_low,L.state);
            }
            /* C4 E rising edge needs setup time since last A0/A1 change */
            if(((cur.e_power_low&&!prev.e_power_low)||(cur.e_reset_low&&!prev.e_reset_low)||(cur.e_kvm_low&&!prev.e_kvm_low)) && (now-a0a1_changed_at) < c.setup_time_ms)
                VIOL(4,"seed%d step%ld: E fell low only %llums after A0/A1 change (setup=%u)",sd,i,(unsigned long long)(now-a0a1_changed_at),c.setup_time_ms);
            /* C5 ATX E low requires current raw LED == active channel */
            if((cur.e_power_low||cur.e_reset_low) && (in.led_raw_state!=cur.active_channel)) VIOL(5,"seed%d step%ld: ATX E low while raw LED=%d active=%d",sd,i,in.led_raw_state,cur.active_channel);
            /* C6 pulse durations */
            if(cur.e_power_low){ if(!p_on){p_on=1;p_start=now;} if(now-p_start>c.max_force_power_pulse_ms+last_dt) VIOL(6,"seed%d step%ld: power pulse %llums",sd,i,(unsigned long long)(now-p_start)); } else p_on=0;
            if(cur.e_reset_low){ if(!r_on){r_on=1;r_start=now;} if(now-r_start>c.max_reset_pulse_ms+last_dt) VIOL(7,"seed%d step%ld: reset pulse %llums",sd,i,(unsigned long long)(now-r_start)); } else r_on=0;
            if(cur.e_kvm_low){ if(!k_on){k_on=1;k_start=now;} if(now-k_start>c.switch_kvm_pulse_time_ms+last_dt) VIOL(8,"seed%d step%ld: kvm pulse %llums",sd,i,(unsigned long long)(now-k_start)); } else k_on=0;
            /* C7 invariant 1: in IDLE, A0/A1 == active channel */
            if(L.state==KVM_STATE_IDLE && cur.a0a1_channel!=cur.active_channel) VIOL(9,"seed%d step%ld: IDLE but a0a1=%d active=%d",sd,i,cur.a0a1_channel,cur.active_channel);
            /* C8 ATX low only in ATX_PULSE; KVM low only in SWITCH_PULSE */
            if((cur.e_power_low||cur.e_reset_low) && L.state!=KVM_STATE_ATX_PULSE) VIOL(10,"seed%d step%ld: ATX E low in state %d",sd,i,L.state);
            if(cur.e_kvm_low && L.state!=KVM_STATE_SWITCH_PULSE) VIOL(11,"seed%d step%ld: KVM E low in state %d",sd,i,L.state);
            prev=cur;
        }
    }

    const char*n[]={"","ATX E low while locked","KVM+ATX E low together","A0/A1 changed while an E was low","E fell low before setup time after A0/A1 change","ATX E low while raw LED != active","power pulse too long","reset pulse too long","kvm pulse too long","IDLE but A0/A1 != active","ATX E low outside ATX_PULSE","KVM E low outside SWITCH_PULSE"};
    int total=0; for(int k=1;k<12;k++){
        if (viol[k]) {
            printf("%-48s : %d%s%s\n",n[k],viol[k],viol[k]?"   e.g. ":"",first[k]);
        }
        total+=viol[k];
    }
    TEST_ASSERT_EQUAL_MESSAGE(0, total, "Fuzz invariant violations found!");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_invariants);
    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
